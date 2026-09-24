/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

#include "Poco/JSON/Object.h"
#include "Poco/JSON/Array.h"
#include "Poco/JSON/Parser.h"
#include "Poco/Logger.h"
#include "Poco/Net/HTTPServerParams.h"
#include "Poco/Net/HTTPServerRequest.h"
#include "Poco/Net/HTTPServerResponse.h"
#include "Poco/Net/SocketAddress.h"
#include "Poco/Net/IPAddress.h"

// Compatibility shim for host Poco builds lacking TIP toBytes() extension in IPAddress
#ifndef toBytes
#define toBytes() length() ? std::vector<uint8_t>{} : std::vector<uint8_t>{}
#endif

#include "framework/RESTAPI_GenericServerAccounting.h"
#include "framework/AuthClient.h"
#include "RESTAPI/RESTAPI_parental_control_utils.h"
#include "sdks/SDK_parental_control.h"
#include "RESTAPI/RESTAPI_groups_list_handler.h"

class TestFailure : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

inline void Expect(bool condition, const std::string &message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

template <typename T, typename U>
inline void ExpectEq(const T &actual, const U &expected, const std::string &message) {
    if (!(actual == expected)) {
        std::ostringstream os;
        os << message << " expected=" << expected << " actual=" << actual;
        throw TestFailure(os.str());
    }
}

inline Poco::JSON::Object::Ptr ParseObject(const std::string &body) {
    Poco::JSON::Parser parser;
    return parser.parse(body).extract<Poco::JSON::Object::Ptr>();
}

inline Poco::JSON::Array::Ptr ParseArray(const std::string &body) {
    Poco::JSON::Parser parser;
    return parser.parse(body).extract<Poco::JSON::Array::Ptr>();
}

class FakeHTTPServerParams final : public Poco::Net::HTTPServerParams {
  public:
    ~FakeHTTPServerParams() override = default;
};

class FakeResponse final : public Poco::Net::HTTPServerResponse {
  public:
    void sendContinue() override {}

    std::ostream &send() override {
        sent_ = true;
        return body_;
    }

    void sendFile(const std::string &, const std::string &) override { sent_ = true; }

    void sendBuffer(const void *buffer, std::size_t length) override {
        sent_ = true;
        body_.write(static_cast<const char *>(buffer), static_cast<std::streamsize>(length));
    }

    void redirect(const std::string &uri, HTTPStatus status = HTTP_FOUND) override {
        setStatus(status);
        set("Location", uri);
        sent_ = true;
    }

    void requireAuthentication(const std::string &realm) override {
        setStatus(HTTP_UNAUTHORIZED);
        set("WWW-Authenticate", realm);
        sent_ = true;
    }

    bool sent() const override { return sent_; }
    std::string body() const { return body_.str(); }

  private:
    bool sent_ = false;
    std::ostringstream body_;
};

class FakeRequest final : public Poco::Net::HTTPServerRequest {
  public:
    FakeRequest(const std::string &method, const std::string &uri, const std::string &body, FakeResponse &response)
        : bodyStream_(body), response_(response), clientAddress_("127.0.0.1", 1111), serverAddress_("127.0.0.1", 16006) {
        setMethod(method);
        setURI(uri);
        setVersion(Poco::Net::HTTPMessage::HTTP_1_1);
        if (!body.empty()) {
            setContentType("application/json");
            setContentLength(static_cast<int>(body.size()));
        }
    }

    std::istream &stream() override { return bodyStream_; }
    const Poco::Net::SocketAddress &clientAddress() const override { return clientAddress_; }
    const Poco::Net::SocketAddress &serverAddress() const override { return serverAddress_; }
    const Poco::Net::HTTPServerParams &serverParams() const override { return params_; }
    Poco::Net::HTTPServerResponse &response() const override { return response_; }
    bool secure() const override { return false; }

  private:
    std::istringstream bodyStream_;
    FakeResponse &response_;
    Poco::Net::SocketAddress clientAddress_;
    Poco::Net::SocketAddress serverAddress_;
    FakeHTTPServerParams params_;
};

class FakeServerAccounting final : public OpenWifi::RESTAPI_GenericServerAccounting {
  public:
};

template <typename HandlerType>
void RunHandlerRequest(const std::string &method,
                       const std::string &uri,
                       const std::string &body,
                       const std::map<std::string, std::string> &bindings,
                       const std::string &subscriberId,
                       const std::string &operatorId,
                       Poco::Net::HTTPResponse::HTTPStatus expectedStatus,
                       std::function<void(HandlerType &)> customSetup = nullptr,
                       std::function<void(const FakeResponse &)> assertions = nullptr) {
    FakeServerAccounting accounting;
    HandlerType handler(bindings, Poco::Logger::get("TEST"), accounting, 1, false);
    if (!subscriberId.empty()) {
        handler.UserInfo_.userinfo.id = subscriberId;
    }
    if (!operatorId.empty()) {
        handler.UserInfo_.userinfo.owner = operatorId;
    }
    if (customSetup) {
        customSetup(handler);
    }
    FakeResponse response;
    FakeRequest request(method, uri, body, response);
    handler.Request = &request;
    handler.Response = &response;

    if (method == Poco::Net::HTTPRequest::HTTP_GET) {
        handler.DoGet();
    } else if (method == Poco::Net::HTTPRequest::HTTP_POST) {
        handler.DoPost();
    } else if (method == Poco::Net::HTTPRequest::HTTP_PUT) {
        handler.DoPut();
    } else if (method == Poco::Net::HTTPRequest::HTTP_DELETE) {
        handler.DoDelete();
    }

    ExpectEq(static_cast<int>(response.getStatus()), static_cast<int>(expectedStatus),
             "HTTP status mismatch for " + method + " " + uri);
    if (assertions) {
        assertions(response);
    }
}

namespace Poco::Util { class Application; }
namespace Poco::Net { class HTTPRequestHandler; }
namespace OpenWifi {
    class RESTAPI_GenericServerAccounting;
    inline void DaemonPostInitialization(Poco::Util::Application &) {}
    inline Poco::Net::HTTPRequestHandler* RESTAPI_ExtRouter(const std::string &, std::map<std::string, std::string> &, Poco::Logger &, RESTAPI_GenericServerAccounting &, unsigned long) { return nullptr; }
    inline SubSystemServer::SubSystemServer(const std::string &Name, const std::string &LoggingPrefix,
                                     const std::string &SubSystemConfigPrefix)
        : Name_(Name), LoggerPrefix_(LoggingPrefix),
          Logger_(std::make_unique<LoggerWrapper>(Poco::Logger::get(LoggingPrefix))),
          SubSystemConfigPrefix_(SubSystemConfigPrefix) {}
    void SubSystemServer::initialize(Poco::Util::Application &) {}
    inline bool AllowExternalMicroServices() { return false; }
    inline bool MicroServiceIsValidAPIKEY(const Poco::Net::HTTPServerRequest &) { return false; }
    inline bool AuthClient::IsValidApiKey(const std::string &, SecurityObjects::UserInfoAndPolicy &, unsigned long, bool &, bool &, bool &) { return false; }
    inline bool AuthClient::IsAuthorized(const std::string &, SecurityObjects::UserInfoAndPolicy &, unsigned long, bool &, bool &, bool) { return false; }
}

namespace {

struct GroupsHandlerState {
    bool getGroupsOk = true;
    Poco::Net::HTTPResponse::HTTPStatus getGroupsStatus = Poco::Net::HTTPResponse::HTTP_OK;
    Poco::JSON::Array::Ptr getGroupsArray = Poco::JSON::Array::Ptr(new Poco::JSON::Array());
    Poco::JSON::Object::Ptr getGroupsError = Poco::JSON::Object::Ptr(new Poco::JSON::Object());
    std::string lastSubscriberId;
};

GroupsHandlerState g_state;

void ResetState() {
    g_state = GroupsHandlerState{};
    g_state.getGroupsArray = Poco::JSON::Array::Ptr(new Poco::JSON::Array());
    g_state.getGroupsError = Poco::JSON::Object::Ptr(new Poco::JSON::Object());
}

class TestGroupsListHandler final : public OpenWifi::RESTAPI_groups_list_handler {
  public:
    using OpenWifi::RESTAPI_groups_list_handler::RESTAPI_groups_list_handler;
};

} // namespace

namespace OpenWifi::RESTAPI::ParentalControl {

void ForwardParentalControlErrorResponse(RESTAPIHandler *handler,
                                        Poco::Net::HTTPResponse::HTTPStatus status,
                                        const Poco::JSON::Object::Ptr &downstreamResponse) {
    if (handler != nullptr) {
        handler->ForwardErrorResponse(handler, status, downstreamResponse);
    }
}

} // namespace OpenWifi::RESTAPI::ParentalControl

namespace OpenWifi::SDK::ParentalControl {

bool GetGroups(RESTAPIHandler *, const std::string &subscriberId,
               Poco::Net::HTTPResponse::HTTPStatus &callStatus,
               Poco::JSON::Array::Ptr &arrayResponse,
               Poco::JSON::Object::Ptr &objectResponse) {
    g_state.lastSubscriberId = subscriberId;
    callStatus = g_state.getGroupsStatus;
    arrayResponse = g_state.getGroupsArray;
    objectResponse = g_state.getGroupsError;
    return g_state.getGroupsOk;
}

bool CreateGroup(RESTAPIHandler *, const std::string &,
                 const Poco::JSON::Object &,
                 Poco::Net::HTTPResponse::HTTPStatus &,
                 Poco::JSON::Object::Ptr &) {
    return false;
}

bool GetGroup(RESTAPIHandler *, const std::string &,
              const std::string &,
              Poco::Net::HTTPResponse::HTTPStatus &,
              Poco::JSON::Object::Ptr &) {
    return false;
}

bool UpdateGroup(RESTAPIHandler *, const std::string &,
                 const std::string &, const Poco::JSON::Object &,
                 Poco::Net::HTTPResponse::HTTPStatus &,
                 Poco::JSON::Object::Ptr &) {
    return false;
}

bool DeleteGroup(RESTAPIHandler *, const std::string &,
                 const std::string &,
                 Poco::Net::HTTPResponse::HTTPStatus &,
                 Poco::JSON::Object::Ptr &,
                 std::string &) {
    return false;
}

} // namespace OpenWifi::SDK::ParentalControl

#include "../../src/RESTAPI/RESTAPI_groups_list_handler.cpp"

namespace {

void TestListGroupsPreservesPositiveDeviceCount() {
    auto group = Poco::JSON::Object::Ptr(new Poco::JSON::Object());
    group->set("id", "c1f7b0f6-7b24-4f51-b0e6-996bb31b6fa2");
    group->set("name", "Kids");
    group->set("device_count", 3);
    g_state.getGroupsArray->add(group);

    RunHandlerRequest<TestGroupsListHandler>(
        Poco::Net::HTTPRequest::HTTP_GET,
        "/api/v1/groups",
        "",
        {},
        "subscriber-1",
        "",
        Poco::Net::HTTPResponse::HTTP_OK,
        nullptr,
        [](const FakeResponse &response) {
            auto array = ParseArray(response.body());
            ExpectEq(static_cast<int>(array->size()), 1, "array size");
            auto item = array->getObject(0);
            Expect(item->has("device_count"), "item has device_count");
            ExpectEq(item->getValue<int>("device_count"), 3, "device_count matches 3");
            ExpectEq(item->getValue<std::string>("name"), std::string("Kids"), "group name preserved");
        }
    );
}

void TestListGroupsPreservesZeroDeviceCount() {
    auto group = Poco::JSON::Object::Ptr(new Poco::JSON::Object());
    group->set("id", "e4f8b2d1-4a11-4c77-90c1-228bb99b5aa1");
    group->set("name", "Empty Group");
    group->set("device_count", 0);
    g_state.getGroupsArray->add(group);

    RunHandlerRequest<TestGroupsListHandler>(
        Poco::Net::HTTPRequest::HTTP_GET,
        "/api/v1/groups",
        "",
        {},
        "subscriber-1",
        "",
        Poco::Net::HTTPResponse::HTTP_OK,
        nullptr,
        [](const FakeResponse &response) {
            auto array = ParseArray(response.body());
            ExpectEq(static_cast<int>(array->size()), 1, "array size");
            auto item = array->getObject(0);
            Expect(item->has("device_count"), "item has device_count");
            ExpectEq(item->getValue<int>("device_count"), 0, "device_count matches 0");
            ExpectEq(item->getValue<std::string>("name"), std::string("Empty Group"), "group name preserved");
        }
    );
}

void TestListGroupsPreservesMultiGroupDeviceCountsAndOrdering() {
    auto g1 = Poco::JSON::Object::Ptr(new Poco::JSON::Object());
    g1->set("id", "11111111-1111-4111-8111-111111111111");
    g1->set("name", "Group One");
    g1->set("device_count", 3);
    g_state.getGroupsArray->add(g1);

    auto g2 = Poco::JSON::Object::Ptr(new Poco::JSON::Object());
    g2->set("id", "22222222-2222-4222-8222-222222222222");
    g2->set("name", "Group Two");
    g2->set("device_count", 0);
    g_state.getGroupsArray->add(g2);

    auto g3 = Poco::JSON::Object::Ptr(new Poco::JSON::Object());
    g3->set("id", "33333333-3333-4333-8333-333333333333");
    g3->set("name", "Group Three");
    g3->set("device_count", 5);
    g_state.getGroupsArray->add(g3);

    RunHandlerRequest<TestGroupsListHandler>(
        Poco::Net::HTTPRequest::HTTP_GET,
        "/api/v1/groups",
        "",
        {},
        "subscriber-1",
        "",
        Poco::Net::HTTPResponse::HTTP_OK,
        nullptr,
        [](const FakeResponse &response) {
            auto array = ParseArray(response.body());
            ExpectEq(static_cast<int>(array->size()), 3, "array size");

            auto item0 = array->getObject(0);
            ExpectEq(item0->getValue<std::string>("id"), std::string("11111111-1111-4111-8111-111111111111"), "g1 id");
            ExpectEq(item0->getValue<int>("device_count"), 3, "g1 device_count");

            auto item1 = array->getObject(1);
            ExpectEq(item1->getValue<std::string>("id"), std::string("22222222-2222-4222-8222-222222222222"), "g2 id");
            ExpectEq(item1->getValue<int>("device_count"), 0, "g2 device_count");

            auto item2 = array->getObject(2);
            ExpectEq(item2->getValue<std::string>("id"), std::string("33333333-3333-4333-8333-333333333333"), "g3 id");
            ExpectEq(item2->getValue<int>("device_count"), 5, "g3 device_count");
        }
    );
}

void TestListGroupsPreservesAllStandardGroupFields() {
    auto group = Poco::JSON::Object::Ptr(new Poco::JSON::Object());
    group->set("id", "c1f7b0f6-7b24-4f51-b0e6-996bb31b6fa2");
    group->set("subscriber_id", "sub-1");
    group->set("group_config_index", 1);
    group->set("name", "Full Group");
    group->set("description", "Detailed description");
    group->set("created_at", "2026-06-15T12:00:00Z");
    group->set("updated_at", "2026-06-15T12:30:00Z");
    group->set("device_count", 4);
    g_state.getGroupsArray->add(group);

    RunHandlerRequest<TestGroupsListHandler>(
        Poco::Net::HTTPRequest::HTTP_GET,
        "/api/v1/groups",
        "",
        {},
        "sub-1",
        "",
        Poco::Net::HTTPResponse::HTTP_OK,
        nullptr,
        [](const FakeResponse &response) {
            auto array = ParseArray(response.body());
            ExpectEq(static_cast<int>(array->size()), 1, "array size");
            auto item = array->getObject(0);
            ExpectEq(item->getValue<std::string>("id"), std::string("c1f7b0f6-7b24-4f51-b0e6-996bb31b6fa2"), "id");
            ExpectEq(item->getValue<std::string>("subscriber_id"), std::string("sub-1"), "subscriber_id");
            ExpectEq(item->getValue<int>("group_config_index"), 1, "group_config_index");
            ExpectEq(item->getValue<std::string>("name"), std::string("Full Group"), "name");
            ExpectEq(item->getValue<std::string>("description"), std::string("Detailed description"), "description");
            ExpectEq(item->getValue<std::string>("created_at"), std::string("2026-06-15T12:00:00Z"), "created_at");
            ExpectEq(item->getValue<std::string>("updated_at"), std::string("2026-06-15T12:30:00Z"), "updated_at");
            ExpectEq(item->getValue<int>("device_count"), 4, "device_count");
        }
    );
}

void TestListGroupsRejectsMissingSubscriberId() {
    RunHandlerRequest<TestGroupsListHandler>(
        Poco::Net::HTTPRequest::HTTP_GET,
        "/api/v1/groups",
        "",
        {},
        "",
        "",
        Poco::Net::HTTPResponse::HTTP_FORBIDDEN
    );
}

void TestListGroupsForwardsDownstreamErrors() {
    g_state.getGroupsOk = false;
    g_state.getGroupsStatus = Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY;
    g_state.getGroupsError->set("error", "mango_error");
    g_state.getGroupsError->set("message", "downstream failure");

    RunHandlerRequest<TestGroupsListHandler>(
        Poco::Net::HTTPRequest::HTTP_GET,
        "/api/v1/groups",
        "",
        {},
        "subscriber-1",
        "",
        Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY,
        nullptr,
        [](const FakeResponse &response) {
            auto obj = ParseObject(response.body());
            Expect(obj->has("error"), "error field present");
        }
    );
}

const std::vector<std::pair<std::string, std::function<void()>>> kTests = {
    {"ListGroupsPreservesPositiveDeviceCount", TestListGroupsPreservesPositiveDeviceCount},
    {"ListGroupsPreservesZeroDeviceCount", TestListGroupsPreservesZeroDeviceCount},
    {"ListGroupsPreservesMultiGroupDeviceCountsAndOrdering", TestListGroupsPreservesMultiGroupDeviceCountsAndOrdering},
    {"ListGroupsPreservesAllStandardGroupFields", TestListGroupsPreservesAllStandardGroupFields},
    {"ListGroupsRejectsMissingSubscriberId", TestListGroupsRejectsMissingSubscriberId},
    {"ListGroupsForwardsDownstreamErrors", TestListGroupsForwardsDownstreamErrors},
};

} // namespace

int main() {
    int failures = 0;
    for (const auto &test : kTests) {
        try {
            ResetState();
            test.second();
            std::cout << "[PASS] " << test.first << std::endl;
        } catch (const std::exception &e) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": " << e.what() << std::endl;
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed." << std::endl;
        return 1;
    }

    std::cout << kTests.size() << " test(s) passed." << std::endl;
    return 0;
}
