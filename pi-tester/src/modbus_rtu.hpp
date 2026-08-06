#ifndef P2_MODBUS_RTU_HPP
#define P2_MODBUS_RTU_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace p2 {

struct ParsedResponse {
    bool ok = false;
    bool is_exception = false;
    std::uint8_t exception_code = 0;
    std::string error;
    std::vector<std::uint16_t> registers;
};

struct TransactionResult {
    bool ok = false;
    std::string error;
    std::vector<std::uint8_t> response;
    std::chrono::microseconds elapsed{0};
};

std::uint16_t modbus_crc16(const std::uint8_t* data, std::size_t size);
void append_modbus_crc(std::vector<std::uint8_t>& frame);
bool has_valid_modbus_crc(const std::vector<std::uint8_t>& frame);

std::vector<std::uint8_t> build_read_input_request(
    std::uint8_t slave, std::uint16_t start_address,
    std::uint16_t register_count);
std::vector<std::uint8_t> build_write_multiple_request(
    std::uint8_t slave, std::uint16_t start_address,
    const std::vector<std::uint16_t>& registers);

ParsedResponse parse_read_input_response(
    const std::vector<std::uint8_t>& frame, std::uint8_t expected_slave,
    std::uint16_t expected_register_count);
ParsedResponse parse_write_multiple_response(
    const std::vector<std::uint8_t>& frame, std::uint8_t expected_slave,
    std::uint16_t expected_start_address,
    std::uint16_t expected_register_count);

std::string bytes_hex(const std::vector<std::uint8_t>& bytes);
std::string bytes_decimal(const std::vector<std::uint8_t>& bytes);
std::string registers_hex(const std::vector<std::uint16_t>& registers);
std::string exception_name(std::uint8_t code);

class SerialPort {
public:
    SerialPort();
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool open_port(const std::string& path, bool kernel_rs485,
                   std::string& error);
    TransactionResult transact(const std::vector<std::uint8_t>& request,
                               std::size_t normal_response_size,
                               std::chrono::milliseconds timeout);
    bool is_open() const;

private:
    int fd_;
};

}  // namespace p2

#endif
