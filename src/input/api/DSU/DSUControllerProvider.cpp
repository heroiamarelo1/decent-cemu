#include "input/api/DSU/DSUControllerProvider.h"
#include "input/api/DSU/PadStreamControl.h"

#include <atomic>
#include <cstring>
#include "input/motion/MotionTrace.h"
#include "input/api/DSU/DSUController.h"
#include "config/CemuConfig.h"
#include "gui/wxgui/MainWindow.h"

#if BOOST_OS_WINDOWS
#include <boost/asio/detail/socket_option.hpp>
#include <winsock2.h>
#elif BOOST_OS_LINUX || BOOST_OS_MACOS
#include <sys/time.h>
#include <sys/socket.h>
#endif

static std::atomic<uint8> g_androidPadEdge{0};

void AndroidPadEdgeSet(uint8 edge)
{
	g_androidPadEdge.store(edge);
}

const char* AndroidPadEdgeName()
{
	switch (g_androidPadEdge.load())
	{
	case 1: return "right";
	case 2: return "left";
	case 3: return "top";
	case 4: return "bottom";
	case 5: return "screen";
	case 6: return "back";
	default: return "unknown";
	}
}

DSUControllerProvider::DSUControllerProvider()
	: base_type(), m_uid(rand()), m_socket(m_io_service)
{
	if (!connect())
	{
		throw std::runtime_error("dsu client can't open the udp connection");
	}

	m_running = true;
	m_reader_thread = std::thread(&DSUControllerProvider::reader_thread, this);
	m_writer_thread = std::thread(&DSUControllerProvider::writer_thread, this);
	request_version();
}

DSUControllerProvider::DSUControllerProvider(const DSUProviderSettings& settings)
	: base_type(settings), m_uid(rand()), m_socket(m_io_service)
{
	if (!connect())
	{
		throw std::runtime_error("dsu client can't open the udp connection");
	}

	m_running = true;
	m_reader_thread = std::thread(&DSUControllerProvider::reader_thread, this);
	m_writer_thread = std::thread(&DSUControllerProvider::writer_thread, this);
	request_version();
}

DSUControllerProvider::~DSUControllerProvider()
{
	if (m_running)
	{
		m_running = false;
		// wake up the reader thread by sending a packet to self
		if (m_socketWakeupEndpoint.port() != 0)
		{
			boost::asio::io_context io_context;
			boost::asio::ip::udp::socket socket(io_context);
			boost::system::error_code ec;
			socket.open(m_socketWakeupEndpoint.protocol(), ec);
			if (!ec)
			{
				std::array<char, 1> data{};
				socket.send_to(boost::asio::buffer(data), m_socketWakeupEndpoint, 0, ec);
			}
			else
				cemuLog_log(LogType::Force, "DSUControllerProvider wakeup failed");
		}
		m_reader_thread.join();
		m_writerJobs.push(nullptr); // wake up writer thread by pushing an empty message
		m_writer_thread.join();
	}
}

std::vector<std::shared_ptr<ControllerBase>> DSUControllerProvider::get_controllers()
{
	std::vector<ControllerPtr> result;

	std::array<uint8_t, kMaxClients> indices;
	for (auto i = 0; i < kMaxClients; ++i)
		indices[i] = get_packet_index(i);

	request_pad_info();
	// WiiMoteDSU only starts sending the pad after a request with no slot flags.
	request_pad_data();

	const auto controller_result = wait_update(indices, 3000);
	for (auto i = 0; i < kMaxClients; ++i)
	{
		if (controller_result[i] && is_connected(i))
			result.emplace_back(std::make_shared<DSUController>(i, m_settings));
	}

	return result;
}

bool DSUControllerProvider::connect()
{
	// already connected?
	if (m_receiver_endpoint.address().to_string() == get_settings().ip && m_receiver_endpoint.port() == get_settings().port)
		return true;

	try
	{
		using namespace boost::asio;

		ip::udp::resolver resolver(m_io_service);
		m_receiver_endpoint = *resolver.resolve(get_settings().ip, fmt::format("{}", get_settings().port)).cbegin();

		if (m_socket.is_open())
			m_socket.close();

		m_socket.open(ip::udp::v4());
		m_socket.bind(ip::udp::endpoint(ip::udp::v4(), 0));
		m_socketWakeupEndpoint = ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), m_socket.local_endpoint().port());
		// reset data
		m_state = {};
		m_prev_state = {};

		// restart threads
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "dsu client connect error: {}", ex.what());
		return false;
	}
}

bool DSUControllerProvider::is_connected(uint8_t index) const
{
	if (index >= kMaxClients)
		return false;

	std::scoped_lock lock(m_mutex[index]);
	if (m_state[index].info.state != DsState::Connected)
		return false;
	// A stopped phone keeps the last "connected" packet. Treat silence as gone.
	const auto age = std::chrono::steady_clock::now() - m_state[index].last_update;
	return age < std::chrono::seconds(1);
}

DSUControllerProvider::ControllerState DSUControllerProvider::get_state(uint8_t index) const
{
	if (index >= kMaxClients)
		return {};

	std::scoped_lock lock(m_mutex[index]);
	return m_state[index];
}

DSUControllerProvider::ControllerState DSUControllerProvider::get_prev_state(uint8_t index) const
{
	if (index >= kMaxClients)
		return {};

	std::scoped_lock lock(m_mutex[index]);
	return m_prev_state[index];
}

std::array<bool, DSUControllerProvider::kMaxClients> DSUControllerProvider::wait_update(
	const std::array<uint8_t, kMaxClients>& indices, size_t timeout) const
{
	std::array<bool, kMaxClients> result{false, false, false, false};

	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
	do
	{
		for (int i = 0; i < kMaxClients; ++i)
		{
			if (result[i])
				continue;

			std::unique_lock lock(m_mutex[i]);
			result[i] = indices[i] < m_state[i].packet_index;
		}

		if (std::all_of(result.cbegin(), result.cend(), [](const bool& v) { return v == true; }))
			break;

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	while (std::chrono::steady_clock::now() < end);

	return result;
}

bool DSUControllerProvider::wait_update(uint8_t index, uint32_t packet_index, size_t timeout) const
{
	if (index >= kMaxClients)
		return false;

	std::unique_lock lock(m_mutex[index]);

	if (packet_index < m_state[index].packet_index)
		return true;

	const auto result = m_wait_cond[index].wait_for(lock, std::chrono::milliseconds(timeout),
	                                                [this, index, packet_index]()
	                                                {
		                                                return packet_index < m_state[index].packet_index;
	                                                });

	return result;
}

uint32_t DSUControllerProvider::get_packet_index(uint8_t index) const
{
	std::scoped_lock lock(m_mutex[index]);
	return m_state[index].packet_index;
}

void DSUControllerProvider::request_version()
{
	auto msg = std::make_unique<VersionRequest>(m_uid);
	m_writerJobs.push(std::move(msg));
}

void DSUControllerProvider::request_pad_info()
{
	auto msg = std::make_unique<ListPorts>(m_uid, 4, std::array<uint8_t, 4>{0, 1, 2, 3});
	m_writerJobs.push(std::move(msg));
}

void DSUControllerProvider::request_pad_info(uint8_t index)
{
	if (index >= kMaxClients)
		return;

	auto msg = std::make_unique<ListPorts>(m_uid, 1, std::array<uint8_t, 4>{index});
	m_writerJobs.push(std::move(msg));
}

void DSUControllerProvider::request_pad_data()
{
	auto msg = std::make_unique<DataRequest>(m_uid);
	m_writerJobs.push(std::move(msg));
}

void DSUControllerProvider::request_pad_data(uint8_t index)
{
	if (index >= kMaxClients)
		return;

	auto msg = std::make_unique<DataRequest>(m_uid, index);
	m_writerJobs.push(std::move(msg));
}

MotionSample DSUControllerProvider::get_motion_sample(uint8_t index) const
{
	if (index >= kMaxClients)
		return MotionSample();
	std::scoped_lock lock(m_mutex[index]);
	return m_state[index].motion_sample;
}


void DSUControllerProvider::reader_thread()
{
	SetThreadName("DSU-reader");
	bool first_read = true;
	while (m_running.load(std::memory_order_relaxed))
	{
		ServerMessage* msg;
		//try
		//{
		std::array<char, 256> recv_buf; // NOLINT(cppcoreguidelines-pro-type-member-init, hicpp-member-init)
		boost::asio::ip::udp::endpoint sender_endpoint;
		boost::system::error_code ec{};
		const size_t len = m_socket.receive_from(boost::asio::buffer(recv_buf), sender_endpoint, 0, ec);
		if (!m_running.load(std::memory_order_relaxed))
			break;
		if (ec)
		{

#ifdef DEBUG_DSU_CLIENT
				printf(" DSUControllerProvider::ReaderThread: exception %s\n", ec.what());
#endif

			// there's probably no server listening on the given address:port
			if (first_read) // workaroud: first read always fails?
				first_read = false;
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				std::this_thread::yield();
			}
			continue;
		}

#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::ReaderThread: received message with len: 0x%llx\n", len);
#endif

		if (len < sizeof(ServerMessage)) // cant be a valid message
			continue;

		msg = (ServerMessage*)recv_buf.data();
		//		}
		//		catch (const std::exception&)
		//		{
		//#ifdef DEBUG_DSU_CLIENT
		//			printf(" DSUControllerProvider::ReaderThread: exception %s\n", ex.what());
		//#endif
		//
		//			// there's probably no server listening on the given address:port
		//			if (first_read) // workaroud: first read always fails?
		//				first_read = false;
		//			else
		//			{
		//				std::this_thread::sleep_for(std::chrono::milliseconds(250));
		//				std::this_thread::yield();
		//			}
		//			continue;
		//		}

		uint8_t index = 0xFF;
		switch (msg->GetMessageType())
		{
		case MessageType::Version:
			{
				const auto rsp = (VersionResponse*)msg;
				if (!rsp->IsValid())
				{
#ifdef DEBUG_DSU_CLIENT
				printf(" DSUControllerProvider::ReaderThread: VersionResponse is invalid!\n");
#endif
					continue;
				}

#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::ReaderThread: server version is: 0x%x\n", rsp->GetVersion());
#endif

				m_server_version = rsp->GetVersion();
				// wdc
				break;
			}
		case MessageType::Information:
			{
				const auto info = (PortInfo*)msg;
				if (!info->IsValid())
				{
#ifdef DEBUG_DSU_CLIENT
				printf(" DSUControllerProvider::ReaderThread: PortInfo is invalid!\n");
#endif
					continue;
				}

				index = info->GetIndex();
				cemu_assert(index < kMaxClients);
#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::ReaderThread: received PortInfo for index %d\n", index);
#endif

				auto& mutex = m_mutex[index];
				std::scoped_lock lock(mutex);
				m_prev_state[index] = m_state[index];
				m_state[index] = *info;
				m_wait_cond[index].notify_all();
				break;
			}
		case MessageType::Data:
			{
				const auto rsp = (DataResponse*)msg;
				if (!rsp->IsValid())
				{
#ifdef DEBUG_DSU_CLIENT
				printf(" DSUControllerProvider::ReaderThread: DataResponse is invalid!\n");
#endif
					continue;
				}

				index = rsp->GetIndex();
				cemu_assert(index < kMaxClients);
#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::ReaderThread: received DataResponse for index %d\n", index);
#endif

				auto& mutex = m_mutex[index];
				std::scoped_lock lock(mutex);
				m_prev_state[index] = m_state[index];
				m_state[index] = *rsp;
                m_state[index].coherent_motion = len >= sizeof(DataResponse)+24 &&
                    std::memcmp(recv_buf.data()+sizeof(DataResponse)+16, "DCM2", 4)==0;
                if (m_state[index].coherent_motion)
                    std::memcpy(&m_state[index].sensor_sequence, recv_buf.data()+sizeof(DataResponse)+20, 4);
                if (len >= sizeof(DataResponse) + 25)
                    PadStream_NotePreset(static_cast<uint8>(recv_buf[sizeof(DataResponse) + 24]));
                m_state[index].has_magnet = false;
				m_state[index].mic_down = false;
				m_state[index].screen_down = false;
				if (len >= sizeof(DataResponse) + sizeof(float) * 3)
				{
					float magnet[3];
					memcpy(magnet, recv_buf.data() + sizeof(DataResponse), sizeof(magnet));
					m_state[index].has_magnet = true;
					m_state[index].magnet[0] = magnet[0];
					m_state[index].magnet[1] = magnet[1];
					m_state[index].magnet[2] = magnet[2];
				}
				if (len >= sizeof(DataResponse) + sizeof(float) * 3 + 2)
				{
					const uint8* extra = reinterpret_cast<const uint8*>(recv_buf.data() + sizeof(DataResponse) + sizeof(float) * 3);
					m_state[index].mic_down = extra[0] != 0;
					m_state[index].screen_down = extra[1] != 0;
					// 0 leaves the option alone. 1 turns it off, 2 turns it on.
					if (len >= sizeof(DataResponse) + sizeof(float) * 3 + 3 && (extra[2] == 1 || extra[2] == 2))
					{
						const bool on = extra[2] == 2;
						if (GetConfig().inverted_sensor_bar.GetValue() != on)
						{
							GetConfig().inverted_sensor_bar = on;
							if (g_mainFrame)
							{
								g_mainFrame->CallAfter([]() {
									if (g_mainFrame)
										g_mainFrame->SyncInvertedSensorBarMenu();
								});
							}
							else
								GetConfigHandle().Save();
						}
					}
					if (len >= sizeof(DataResponse) + sizeof(float) * 3 + 4)
						AndroidPadEdgeSet(extra[3]);
				}
				m_wait_cond[index].notify_all();
				// update motion info immediately, guaranteeing that we dont drop packets
				integrate_motion(index, *rsp);
				break;
			}
		}

		request_pad_data();
		if (index != 0xFF)
			request_pad_data(index);
	}
}

void DSUControllerProvider::writer_thread()
{
	SetThreadName("DSU-writer");
	while (m_running.load(std::memory_order_relaxed))
	{
		std::unique_ptr<ClientMessage> msg = m_writerJobs.pop();
		if (!m_running.load(std::memory_order_relaxed))
			return;
		cemu_assert_debug(msg.get());
#ifdef DEBUG_DSU_CLIENT
		printf(" DSUControllerProvider::WriterThread: sending message: 0x%x (len: 0x%x)\n", (int)msg->GetMessageType(), msg->GetSize());
#endif
		try
		{
			m_socket.send_to(boost::asio::buffer(msg.get(), msg->GetSize()), m_receiver_endpoint);
		}
		catch (const std::exception& ec)
		{
#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::WriterThread: exception %s\n", ec.what());
#endif
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
	}
}

void DSUControllerProvider::integrate_motion(uint8_t index, const DataResponse& data_response)
{
    const auto& inputAcc = data_response.GetAcceleration();
    const auto& inputGyro = data_response.GetGyro();
    for (const float value : {inputAcc.x, inputAcc.y, inputAcc.z, inputGyro.x, inputGyro.y, inputGyro.z})
        if (!std::isfinite(value)) return;
    const uint64 ts = data_response.GetMotionTimestamp();
    if (!ts) return; // app has not acquired a coherent sensor pair yet
    const auto previous = m_last_motion_timestamp[index];
    if (ts <= previous)
    {
        if (previous-ts >= 10000000)
        {
            m_last_motion_timestamp[index]=0;
            m_motion_handler[index]=WiiUMotionHandler{};
        }
        return;
    }
    m_last_motion_timestamp[index] = ts;
    const bool coherent = m_state[index].coherent_motion;
    const double elapsedTimeD = previous && ts-previous <= 200000 ? double(ts-previous)/1000000.0 : 0.0;
    m_motion_handler[index].setCoherentMotion(coherent);
    m_motion_handler[index].setSampleTime(double(ts)/1000000.0);
	const auto& acc = data_response.GetAcceleration();
	const auto& gyro = data_response.GetGyro();

	m_motion_handler[index].processMotionSample((float)elapsedTimeD,
	                                            gyro.x * 0.0174533f,
	                                            gyro.y * 0.0174533f,
	                                            gyro.z * 0.0174533f,
	                                            acc.x,
	                                            -acc.y,
	                                            -acc.z);

	m_state[index].motion_sample = m_motion_handler[index].getMotionSample();
	// Our Android GamePad app advertises this fixed, locally administered ID.
	// Its screen-right acceleration needs the opposite VPAD lateral sign.
	// Adapt the API output only: reflecting the fusion input would also reverse
	// the already calibrated gyro/camera controls. Other DSU devices retain
	// their existing convention.
	constexpr MACAddress_t androidGamePadId{0x02, 0x67, 0x60, 0x00, 0x00, 0x01};
    // Only the identified DCM2 Android app opts into the complete VPAD frame.
    // Generic DSU, SDL and real Wiimote adapters retain their existing outputs.
    m_state[index].motion_sample.setAndroidVPADFrame(
        data_response.GetMacAddress() == androidGamePadId && coherent);
	m_state[index].motion_sample.setVPADAccelerometerXInverted(
		data_response.GetMacAddress() == androidGamePadId && !coherent);
    const auto q=m_state[index].motion_sample.getQuaternion();
    motion_trace::values("sensor", index, 0, 0, {
        double(ts), double(m_state[index].sensor_sequence), double(coherent), elapsedTimeD,
        acc.x, acc.y, acc.z, gyro.x, gyro.y, gyro.z, q.w, q.x, q.y, q.z,
        double(m_state[index].has_magnet), m_state[index].magnet[0], m_state[index].magnet[1], m_state[index].magnet[2]});

    motion_trace::write("sensor", "%u,%llu,%u,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
        unsigned(index), static_cast<unsigned long long>(ts), m_state[index].sensor_sequence, int(coherent), elapsedTimeD,
        double(acc.x),double(acc.y),double(acc.z),double(gyro.x),double(gyro.y),double(gyro.z),
        double(q.w),double(q.x),double(q.y),double(q.z));
}

DSUControllerProvider::ControllerState& DSUControllerProvider::ControllerState::operator=(const PortInfo& port_info)
{
	info = port_info.GetInfo();
	last_update = std::chrono::steady_clock::now();
	packet_index++; // increase packet index for every packet we assign/recv
	return *this;
}

DSUControllerProvider::ControllerState& DSUControllerProvider::ControllerState::operator=(
	const DataResponse& data_response)
{
	this->operator=(static_cast<const PortInfo&>(data_response));
	data = data_response.GetData();
	return *this;
}