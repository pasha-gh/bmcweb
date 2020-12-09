#pragma once

#include <filesystem>
#include <string_view>
#include <variant>
#include <unistd.h>
#include <atomic>
#include <memory>

#include <boost/asio.hpp>
#include <boost/asio/basic_socket_acceptor.hpp>
#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/beast/http.hpp>

namespace crow
{
namespace dbuscapture
{

static constexpr std::string_view DUMP_PATH = "/usr/share/www/dbus_capture.json";
static constexpr int socketBufferSize = 1024 * 1024;
  
using boost::asio::local::stream_protocol;
  
class Handler : public std::enable_shared_from_this<Handler>
{
  public:
    Handler(boost::asio::io_context& io_context) :
        io_context_(&io_context)
    {}
  
  /**
   * @brief  Reads the dump file and connects its file descriptor
   *         with the stream descriptor.
   *         If dump file does not exist, closes the connection
   *         and returns false.
   *
   * @return bool
   */
    bool readDump() {
        if (std::filesystem::exists(DUMP_PATH)) {
            int fd = open(DUMP_PATH.data(), O_RDONLY);
            psd = new boost::asio::posix::stream_descriptor(*io_context_);
            psd->assign(fd);
            psd->non_blocking(true);
            outputBuffer = new boost::beast::flat_static_buffer<socketBufferSize>(); //TODO make unique
            return true;
        } else {
            this->connection->streamres.result(
                boost::beast::http::status::internal_server_error);
            this->connection->close();
            return false;
        }
    }
  
    /**
   * @brief  Sends the dump.
   *         
   * @todo   Shouldn't we follow dump_offload.hpp standards
   * 
   * @return void
   */
    void sendDump() {
        try {
            std::size_t read_size = outputBuffer->capacity() - outputBuffer->size();    
            std::size_t count = psd->read_some(boost::asio::buffer(buf,
              read_size));
            boost::asio::buffer_copy(outputBuffer->prepare(count), boost::asio::buffer(buf));
            
            outputBuffer->commit(count);
            std::string_view payload(
              static_cast<const char*>(outputBuffer->data().data()),
              count);
              auto streamHandler = [this, count, self(shared_from_this())]() {
              this->outputBuffer->consume(count);
              this->sendDump();
            };
            this->connection->sendMessage(payload, streamHandler);
        } catch (const boost::system::system_error& e) {
            this->connection->close();
            return;
        }
    }

    /**
     * @brief  Resets output buffers.
     * @return void
     */
    void resetBuffers()
    {
        this->outputBuffer->clear();
    }

    boost::asio::io_context* io_context_;
    crow::streamsocket::Connection* connection = nullptr;
    boost::beast::flat_static_buffer<socketBufferSize>* outputBuffer;
    boost::asio::posix::stream_descriptor* psd;
    unsigned char buf[socketBufferSize];
};

static boost::container::flat_map<crow::streamsocket::Connection*,
                                  std::shared_ptr<Handler>> handlers;
void requestRoutes(App& app)
{
  BMCWEB_ROUTE(app, "/redfish/v1/System/dbus/")
      .methods(boost::beast::http::verb::get)(
          [](const crow::Request&, crow::Response& res) {
              res.jsonValue = {{""}};
              res.end();
          });
  BMCWEB_ROUTE(app, "/redfish/v1/System/dbus/GetCapture/")
  .privileges({"ConfigureComponents", "ConfigureManager"})
  .streamsocket()
  .onopen([](crow::streamsocket::Connection& conn) {
      boost::asio::io_context* ioCon = conn.getIoContext();
      handlers[&conn] = std::make_shared<Handler>(*ioCon);
      handlers[&conn]->connection = &conn;
      if (handlers[&conn]->readDump()) {
          handlers[&conn]->sendDump();
      }
  })
  .onclose([](crow::streamsocket::Connection& conn) {
      auto itr = handlers.find(&conn);
      if (itr == handlers.end()) {
          return;
      }
      handlers.erase(itr);
      itr->second->outputBuffer->clear();
  });
}

} // namespace dbuscapture
} // namespace crow