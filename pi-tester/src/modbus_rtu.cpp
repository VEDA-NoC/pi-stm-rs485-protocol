#include "modbus_rtu.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace p2 {
namespace {

constexpr std::uint8_t kFcReadInputRegisters = 4;
constexpr std::uint8_t kFcWriteMultipleRegisters = 16;

void push_u16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& input,
                       std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input.at(offset)) << 8) |
        static_cast<std::uint16_t>(input.at(offset + 1)));
}

ParsedResponse validate_common(const std::vector<std::uint8_t>& frame,
                               std::uint8_t expected_slave,
                               std::uint8_t expected_function)
{
    ParsedResponse result;
    if (frame.size() < 5) {
        result.error = "응답이 Modbus RTU 최소 길이 5 bytes보다 짧습니다.";
        return result;
    }
    if (!has_valid_modbus_crc(frame)) {
        result.error = "응답 CRC16이 일치하지 않습니다.";
        return result;
    }
    if (frame[0] != expected_slave) {
        result.error = "응답 slave address가 요청과 다릅니다.";
        return result;
    }
    if (frame[1] == static_cast<std::uint8_t>(expected_function | 0x80U)) {
        result.is_exception = true;
        result.exception_code = frame[2];
        result.error = "STM이 Modbus exception " +
                       std::to_string(result.exception_code) + " (" +
                       exception_name(result.exception_code) + ")을 반환했습니다.";
        return result;
    }
    if (frame[1] != expected_function) {
        result.error = "응답 function code가 요청과 다릅니다.";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace

std::uint16_t modbus_crc16(const std::uint8_t* data, std::size_t size)
{
    std::uint16_t crc = 0xffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xa001U);
            } else {
                crc = static_cast<std::uint16_t>(crc >> 1);
            }
        }
    }
    return crc;
}

void append_modbus_crc(std::vector<std::uint8_t>& frame)
{
    const std::uint16_t crc = modbus_crc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc & 0xffU));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8));
}

bool has_valid_modbus_crc(const std::vector<std::uint8_t>& frame)
{
    if (frame.size() < 2) {
        return false;
    }
    const std::size_t payload_size = frame.size() - 2;
    const std::uint16_t received =
        static_cast<std::uint16_t>(frame[payload_size]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(frame[payload_size + 1]) << 8);
    return received == modbus_crc16(frame.data(), payload_size);
}

std::vector<std::uint8_t> build_read_input_request(
    std::uint8_t slave, std::uint16_t start_address,
    std::uint16_t register_count)
{
    if (slave == 0 || slave > 247) {
        throw std::invalid_argument("read slave address는 1..247이어야 합니다.");
    }
    if (register_count == 0 || register_count > 125) {
        throw std::invalid_argument("FC4 register count는 1..125여야 합니다.");
    }
    std::vector<std::uint8_t> frame{slave, kFcReadInputRegisters};
    push_u16(frame, start_address);
    push_u16(frame, register_count);
    append_modbus_crc(frame);
    return frame;
}

std::vector<std::uint8_t> build_write_multiple_request(
    std::uint8_t slave, std::uint16_t start_address,
    const std::vector<std::uint16_t>& registers)
{
    if (slave == 0 || slave > 247) {
        throw std::invalid_argument("write slave address는 1..247이어야 합니다.");
    }
    if (registers.empty() || registers.size() > 123) {
        throw std::invalid_argument("FC16 register count는 1..123이어야 합니다.");
    }
    std::vector<std::uint8_t> frame{slave, kFcWriteMultipleRegisters};
    push_u16(frame, start_address);
    push_u16(frame, static_cast<std::uint16_t>(registers.size()));
    frame.push_back(
        static_cast<std::uint8_t>(registers.size() * sizeof(std::uint16_t)));
    for (const std::uint16_t value : registers) {
        push_u16(frame, value);
    }
    append_modbus_crc(frame);
    return frame;
}

ParsedResponse parse_read_input_response(
    const std::vector<std::uint8_t>& frame, std::uint8_t expected_slave,
    std::uint16_t expected_register_count)
{
    ParsedResponse result =
        validate_common(frame, expected_slave, kFcReadInputRegisters);
    if (!result.ok) {
        return result;
    }
    const std::size_t expected_size =
        5U + static_cast<std::size_t>(expected_register_count) * 2U;
    if (frame.size() != expected_size) {
        result.ok = false;
        result.error = "FC4 응답 전체 길이가 요청 register 수와 맞지 않습니다.";
        return result;
    }
    const std::uint8_t expected_byte_count =
        static_cast<std::uint8_t>(expected_register_count * 2U);
    if (frame[2] != expected_byte_count) {
        result.ok = false;
        result.error = "FC4 byte count가 요청 register 수와 맞지 않습니다.";
        return result;
    }
    for (std::uint16_t i = 0; i < expected_register_count; ++i) {
        result.registers.push_back(read_u16(frame, 3U + i * 2U));
    }
    return result;
}

ParsedResponse parse_write_multiple_response(
    const std::vector<std::uint8_t>& frame, std::uint8_t expected_slave,
    std::uint16_t expected_start_address,
    std::uint16_t expected_register_count)
{
    ParsedResponse result = validate_common(
        frame, expected_slave, kFcWriteMultipleRegisters);
    if (!result.ok) {
        return result;
    }
    if (frame.size() != 8) {
        result.ok = false;
        result.error = "FC16 정상 응답은 8 bytes여야 합니다.";
        return result;
    }
    if (read_u16(frame, 2) != expected_start_address ||
        read_u16(frame, 4) != expected_register_count) {
        result.ok = false;
        result.error = "FC16 응답의 시작 주소 또는 register 수가 요청과 다릅니다.";
        return result;
    }
    return result;
}

std::string bytes_hex(const std::vector<std::uint8_t>& bytes)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return stream.str();
}

std::string bytes_decimal(const std::vector<std::uint8_t>& bytes)
{
    std::ostringstream stream;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            stream << ' ';
        }
        stream << static_cast<unsigned>(bytes[i]);
    }
    return stream.str();
}

std::string registers_hex(const std::vector<std::uint16_t>& registers)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < registers.size(); ++i) {
        if (i != 0) {
            stream << " | ";
        }
        stream << std::setw(4) << registers[i];
    }
    return stream.str();
}

std::string exception_name(std::uint8_t code)
{
    switch (code) {
        case 1:
            return "Illegal Function";
        case 2:
            return "Illegal Data Address";
        case 3:
            return "Illegal Data Value";
        case 4:
            return "Slave Device Failure";
        default:
            return "unknown";
    }
}

SerialPort::SerialPort() : fd_(-1) {}

SerialPort::~SerialPort()
{
#if defined(__linux__)
    if (fd_ >= 0) {
        ::close(fd_);
    }
#endif
}

bool SerialPort::open_port(const std::string& path, bool kernel_rs485,
                           std::string& error)
{
#if defined(__linux__)
    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        error = "serial open 실패: " + std::string(std::strerror(errno));
        return false;
    }

    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
        error = "tcgetattr 실패: " + std::string(std::strerror(errno));
        return false;
    }
    cfmakeraw(&tty);
    if (cfsetispeed(&tty, B19200) != 0 ||
        cfsetospeed(&tty, B19200) != 0) {
        error = "19200 baud 설정 실패: " +
                std::string(std::strerror(errno));
        return false;
    }
    tty.c_cflag &= static_cast<tcflag_t>(~CSIZE);
    tty.c_cflag |= static_cast<tcflag_t>(CS8 | CLOCAL | CREAD | PARENB);
    tty.c_cflag &= static_cast<tcflag_t>(~(PARODD | CSTOPB | CRTSCTS));
    tty.c_iflag |= INPCK;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        error = "tcsetattr 19200 8E1 설정 실패: " +
                std::string(std::strerror(errno));
        return false;
    }

    if (kernel_rs485) {
        serial_rs485 config{};
        config.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
        if (ioctl(fd_, TIOCSRS485, &config) != 0) {
            error = "TIOCSRS485 설정 실패: " +
                    std::string(std::strerror(errno)) +
                    ". USB 자동 방향 제어 adapter라면 --kernel-rs485를 빼십시오.";
            return false;
        }
    }
    return true;
#else
    (void)path;
    (void)kernel_rs485;
    error = "실제 serial 송수신은 Linux/Raspberry Pi에서만 지원합니다. "
            "이 환경에서는 --dry-run 또는 unit test를 사용하십시오.";
    return false;
#endif
}

TransactionResult SerialPort::transact(
    const std::vector<std::uint8_t>& request,
    std::size_t normal_response_size, std::chrono::milliseconds timeout)
{
    TransactionResult result;
#if defined(__linux__)
    if (fd_ < 0) {
        result.error = "serial port가 열리지 않았습니다.";
        return result;
    }
    tcflush(fd_, TCIFLUSH);
    const auto started = std::chrono::steady_clock::now();
    std::size_t written = 0;
    while (written < request.size()) {
        const ssize_t count =
            ::write(fd_, request.data() + written, request.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno != EAGAIN && errno != EINTR) {
            result.error = "serial write 실패: " +
                           std::string(std::strerror(errno));
            return result;
        }
        pollfd output_poll{fd_, POLLOUT, 0};
        if (poll(&output_poll, 1, static_cast<int>(timeout.count())) <= 0) {
            result.error = "serial write timeout";
            return result;
        }
    }
    if (tcdrain(fd_) != 0) {
        result.error = "tcdrain 실패: " + std::string(std::strerror(errno));
        return result;
    }

    const auto response_wait_started = std::chrono::steady_clock::now();
    const auto deadline = response_wait_started + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd input_poll{fd_, POLLIN, 0};
        const int poll_result =
            poll(&input_poll, 1, std::max(1, static_cast<int>(remaining.count())));
        if (poll_result < 0 && errno != EINTR) {
            result.error = "serial poll 실패: " +
                           std::string(std::strerror(errno));
            return result;
        }
        if (poll_result <= 0) {
            continue;
        }
        std::uint8_t buffer[256];
        const ssize_t count = ::read(fd_, buffer, sizeof(buffer));
        if (count > 0) {
            result.response.insert(result.response.end(), buffer,
                                   buffer + count);
            const bool exception_complete =
                result.response.size() >= 5 &&
                (result.response[1] & 0x80U) != 0U;
            if (exception_complete ||
                result.response.size() >= normal_response_size) {
                result.ok = true;
                result.elapsed =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started);
                return result;
            }
        }
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    result.error = "response timeout (" + std::to_string(timeout.count()) +
                   "ms), received " +
                   std::to_string(result.response.size()) + " bytes";
#else
    (void)request;
    (void)normal_response_size;
    (void)timeout;
    result.error = "serial transaction은 Linux에서만 지원합니다.";
#endif
    return result;
}

bool SerialPort::is_open() const
{
    return fd_ >= 0;
}

}  // namespace p2
