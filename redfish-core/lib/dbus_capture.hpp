/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#pragma once

#include "node.hpp"
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <fstream>
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

#include <sdbusplus/bus.hpp>

namespace redfish
{
namespace dbuscapture
{
    constexpr std::string_view DUMP_PATH = "/usr/share/www/dbus_capture.json";
    std::thread* dbus_cap_thd = nullptr;
    std::atomic<bool> is_capturing_dbus = false;
    sd_bus* g_bus = nullptr;
    bool IS_USER_BUS = false;

    int AcquireBus() {
        int r; // TODO send errors after each step
        r = sd_bus_new(&g_bus); 
        r = sd_bus_set_monitor(g_bus, true);
        r = sd_bus_negotiate_creds(g_bus, true, _SD_BUS_CREDS_ALL);
        r = sd_bus_negotiate_timestamp(g_bus, true);
        r = sd_bus_negotiate_fds(g_bus, true);
        r = sd_bus_set_bus_client(g_bus, true);
        if (IS_USER_BUS) {
            r = sd_bus_set_address(g_bus, "bmcwebDbusAddress");
        } else {
            r = sd_bus_set_address(g_bus, "unix:path=/run/dbus/system_bus_socket");
        }
        r = sd_bus_start(g_bus);

        return r;
    }

    int BecomeDBusMonitor() {
        uint32_t flags = 0;
        int r;
        AcquireBus();
        sd_bus_message* message;
        sd_bus_error error = SD_BUS_ERROR_NULL;
        r = sd_bus_message_new_method_call(g_bus, &message,
                        "org.freedesktop.DBus",
                        "/org/freedesktop/DBus",
                        "org.freedesktop.DBus.Monitoring",
                        "BecomeMonitor");
        r = sd_bus_message_open_container(message, 'a', "s");
        r = sd_bus_message_close_container(message);
        r = sd_bus_message_append_basic(message, 'u', &flags);
        r = sd_bus_call(g_bus, message, 0, &error, nullptr);
        const char* unique_name;
        r = sd_bus_get_unique_name(g_bus, &unique_name);
        return r;
    }
    
    void WriteToCaptureDump(nlohmann::json j) {
        std::fstream uidlFile(std::string(DUMP_PATH).data(), std::fstream::in | std::fstream::out | std::fstream::app);
        if (uidlFile.is_open())
        {
            uidlFile << std::setw(1) << j << "," << std::endl;
            uidlFile.close();
        }
    }

    void Capture() {
        is_capturing_dbus = true;
        BecomeDBusMonitor();
        while (is_capturing_dbus) {
            struct sd_bus_message*msg = nullptr;
            nlohmann::json j;
            int r;
            
            sd_bus_process(g_bus, &msg);

            uint8_t type;
            r = sd_bus_message_get_type(msg, &type);
            if (r >= 0)
            {
                j["type"] = std::to_string(type);
            }

            uint64_t cookie;
            r = sd_bus_message_get_cookie(msg, &cookie);
            if (r >= 0)
            {
                j["cookie"] = std::to_string(cookie);
            }

            uint64_t reply_cookie;
            r = sd_bus_message_get_reply_cookie(msg, &reply_cookie);
            if (r >= 0)
            {
                j["reply_cookie"] = std::to_string(reply_cookie);
            }

            const char *path = sd_bus_message_get_path(msg);
            if (path)
            {
                j["path"] = path;
            }

            const char *interface = sd_bus_message_get_interface(msg);
            if (interface)
            {
                j["interface"] = interface;
            }

            const char *sender = sd_bus_message_get_sender(msg);
            if (sender)
            {
                j["sender"] = sender;
            }

            const char *destination = sd_bus_message_get_destination(msg);
            if (destination)
            {
                j["destination"] = destination;
            }

            const char *member = sd_bus_message_get_member(msg);
            if (member)
            {
                j["member"] = member;
            }

            const char *signature = sd_bus_message_get_signature(msg, true);
            if (signature)
            {
                j["member"] = signature;
            }

            auto usec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            j["time"] = std::to_string(usec);

            WriteToCaptureDump(j);
            sd_bus_wait(g_bus, std::numeric_limits<uint64_t>::max());
        }
    }
}

class DBusCaptureService : public Node
{
  public:
    DBusCaptureService(App& app) :
        Node(app, "/redfish/v1/Systems/dbus/DBusCapture/")
    {
        entityPrivileges = {
            { boost::beast::http::verb::get, {{"Login"}}},
            { boost::beast::http::verb::head, {{"Login"}}},
            { boost::beast::http::verb::patch, {{"ConfigureManager"}}},
            { boost::beast::http::verb::put, {{"ConfigureManager"}}},
            { boost::beast::http::verb::delete_, {{"ConfigureManager"}}},
            { boost::beast::http::verb::post, {{"ConfigureManager"}}}};
    }
private:
  void doGet(crow::Response& res, const crow::Request&,
           const std::vector<std::string>&) override
  {
    std::shared_ptr<redfish::AsyncResp> asyncResp =
        std::make_shared<redfish::AsyncResp>(res);
    
    asyncResp->res.jsonValue = { 
        { "isCapturing" },
        { std::to_string(dbuscapture::dbus_cap_thd == nullptr) }
    };
    
  }
};

class DBusCaptureStart : public Node
{
  public:
    DBusCaptureStart(App& app) :
        Node(app, "/redfish/v1/Systems/dbus/DBusCapture/"
                  "Actions/"
                  "DbusCapture.StartCapture/")
    {
        entityPrivileges = {
            { boost::beast::http::verb::get, {{"Login"}}},
            { boost::beast::http::verb::head, {{"Login"}}},
            { boost::beast::http::verb::patch, {{"ConfigureManager"}}},
            { boost::beast::http::verb::put, {{"ConfigureManager"}}},
            { boost::beast::http::verb::delete_, {{"ConfigureManager"}}},
            { boost::beast::http::verb::post, {{"ConfigureManager"}}}};
    }

  private:
    void doGet(crow::Response& res, const crow::Request&,
               const std::vector<std::string>&) override
    {
        std::shared_ptr<redfish::AsyncResp> asyncResp =
            std::make_shared<redfish::AsyncResp>(res);
        if (dbuscapture::dbus_cap_thd == nullptr) {
            dbuscapture::dbus_cap_thd = new std::thread(dbuscapture::Capture);
        }
        asyncResp->res.jsonValue = { 
            { "isCapturing" },
            { std::to_string(dbuscapture::dbus_cap_thd == nullptr) }
        };
    }
};

class DBusCaptureStop : public Node
{
  public:
    DBusCaptureStop(App& app) :
        Node(app, "/redfish/v1/Systems/dbus/DBusCapture/"
                  "Actions/"
                  "DbusCapture.StopCapture/")
    {
        entityPrivileges = {
            { boost::beast::http::verb::get,     {{"Login"}}},
            { boost::beast::http::verb::head,    {{"Login"}}},
            { boost::beast::http::verb::patch,   {{"ConfigureManager"}}},
            { boost::beast::http::verb::put,     {{"ConfigureManager"}}},
            { boost::beast::http::verb::delete_, {{"ConfigureManager"}}},
            { boost::beast::http::verb::post,    {{"ConfigureManager"}}}};
    }
  private:
    void doGet(crow::Response& res, const crow::Request&,
       const std::vector<std::string>&) override
    {
        std::shared_ptr<redfish::AsyncResp> asyncResp =
            std::make_shared<redfish::AsyncResp>(res);
        if (!(dbuscapture::dbus_cap_thd == nullptr)) {
            dbuscapture::is_capturing_dbus = false;
            dbuscapture::dbus_cap_thd->join();
            dbuscapture::dbus_cap_thd = nullptr;
            
        }
        asyncResp->res.jsonValue = { 
            { "isCapturing" },
            { std::to_string(dbuscapture::dbus_cap_thd == nullptr) }
        };
    }
};

class DBusCaptureClear : public Node
{
  public:
    DBusCaptureClear(App& app) :
        Node(app, "/redfish/v1/Systems/dbus/DBusCapture/"
                  "Actions/"
                  "DbusCapture.Clear/")
    {
        entityPrivileges = {
            { boost::beast::http::verb::get,     {{"Login"}}},
            { boost::beast::http::verb::head,    {{"Login"}}},
            { boost::beast::http::verb::patch,   {{"ConfigureManager"}}},
            { boost::beast::http::verb::put,     {{"ConfigureManager"}}},
            { boost::beast::http::verb::delete_, {{"ConfigureManager"}}},
            { boost::beast::http::verb::post,    {{"ConfigureManager"}}}};
    }
  private:
    void doGet(crow::Response& res, const crow::Request&,
       const std::vector<std::string>&) override
    {
        std::shared_ptr<redfish::AsyncResp> asyncResp =
            std::make_shared<redfish::AsyncResp>(res);
        std::filesystem::resize_file(dbuscapture::DUMP_PATH, 0);
        asyncResp->res.jsonValue = { { "Message" }, { "Capture not started" } };
    }
};

} // namespace redfish
