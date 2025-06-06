#include <Server/PrometheusRequestHandler.h>

#include <IO/HTTPCommon.h>
#include <Server/HTTP/WriteBufferFromHTTPServerResponse.h>
#include <Server/HTTP/sendExceptionToHTTPClient.h>
#include <Server/HTTPHandler.h>
#include <Server/IServer.h>
#include <Server/PrometheusMetricsWriter.h>
#include <base/scope_guard.h>
#include <Poco/Net/HTTPResponse.h>
#include <Common/logger_useful.h>
#include <Common/setThreadName.h>
#include "config.h"

#include <Access/Credentials.h>
#include <Common/CurrentThread.h>
#include <IO/SnappyReadBuffer.h>
#include <IO/SnappyWriteBuffer.h>
#include <IO/Protobuf/ProtobufZeroCopyInputStreamFromReadBuffer.h>
#include <IO/Protobuf/ProtobufZeroCopyOutputStreamFromWriteBuffer.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/Session.h>
#include <Server/HTTP/HTMLForm.h>
#include <Server/HTTP/authenticateUserByHTTP.h>
#include <Server/HTTP/checkHTTPHeader.h>
#include <Server/HTTP/setReadOnlyIfHTTPMethodIdempotent.h>
#include <Storages/TimeSeries/PrometheusRemoteReadProtocol.h>
#include <Storages/TimeSeries/PrometheusRemoteWriteProtocol.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int SUPPORT_IS_DISABLED;
}

/// Base implementation of a prometheus protocol.
class PrometheusRequestHandler::Impl
{
public:
    explicit Impl(PrometheusRequestHandler & parent) : parent_ref(parent) {}
    virtual ~Impl() = default;
    virtual void beforeHandlingRequest(HTTPServerRequest & /* request */) {}
    virtual void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response) = 0;
    virtual void onException() {}

protected:
    PrometheusRequestHandler & parent() { return parent_ref; }
    IServer & server() { return parent().server; }
    const PrometheusRequestHandlerConfig & config() { return parent().config; }
    PrometheusMetricsWriter & metrics_writer() { return *parent().metrics_writer; }
    LoggerPtr log() { return parent().log; }
    WriteBuffer & getOutputStream(HTTPServerResponse & response) { return parent().getOutputStream(response); }

private:
    PrometheusRequestHandler & parent_ref;
};


/// Implementation of the exposing metrics protocol.
class PrometheusRequestHandler::ExposeMetricsImpl : public Impl
{
public:
    explicit ExposeMetricsImpl(PrometheusRequestHandler & parent) : Impl(parent) {}

    void beforeHandlingRequest(HTTPServerRequest & request) override
    {
        LOG_INFO(log(), "Handling metrics request from {}", request.get("User-Agent"));
        chassert(config().type == PrometheusRequestHandlerConfig::Type::ExposeMetrics);
    }

    void handleRequest(HTTPServerRequest & /* request */, HTTPServerResponse & response) override
    {
        response.setContentType("text/plain; version=0.0.4; charset=UTF-8");
        auto & out = getOutputStream(response);

        if (config().expose_events)
            metrics_writer().writeEvents(out);

        if (config().expose_metrics)
            metrics_writer().writeMetrics(out);

        if (config().expose_asynchronous_metrics)
            metrics_writer().writeAsynchronousMetrics(out, parent().async_metrics);

        if (config().expose_errors)
            metrics_writer().writeErrors(out);
    }
};


/// Base implementation of a protocol with Context and authentication.
class PrometheusRequestHandler::ImplWithContext : public Impl
{
public:
    explicit ImplWithContext(PrometheusRequestHandler & parent) : Impl(parent), default_settings(server().context()->getSettingsRef()) { }

    virtual void handlingRequestWithContext(HTTPServerRequest & request, HTTPServerResponse & response) = 0;

protected:
    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response) override
    {
        SCOPE_EXIT({
            request_credentials.reset();
            context.reset();
            session.reset();
            params.reset();
        });

        params = std::make_unique<HTMLForm>(default_settings, request);
        parent().send_stacktrace = config().is_stacktrace_enabled && params->getParsed<bool>("stacktrace", false);

        if (!authenticateUserAndMakeContext(request, response))
            return; /// The user is not authenticated yet, and the HTTP_UNAUTHORIZED response is sent with the "WWW-Authenticate" header,
                    /// and `request_credentials` must be preserved until the next request or until any exception.

        /// Initialize query scope.
        std::optional<CurrentThread::QueryScope> query_scope;
        if (context)
            query_scope.emplace(context);

        handlingRequestWithContext(request, response);
    }

    bool authenticateUserAndMakeContext(HTTPServerRequest & request, HTTPServerResponse & response)
    {
        session = std::make_unique<Session>(server().context(), ClientInfo::Interface::PROMETHEUS, request.isSecure());

        if (!authenticateUser(request, response))
            return false;

        makeContext(request);
        return true;
    }

    bool authenticateUser(HTTPServerRequest & request, HTTPServerResponse & response)
    {
        return authenticateUserByHTTP(request, *params, response, *session, request_credentials, config().connection_config, server().context(), log());
    }

    void makeContext(HTTPServerRequest & request)
    {
        context = session->makeQueryContext();

        /// Anything else beside HTTP POST should be readonly queries.
        setReadOnlyIfHTTPMethodIdempotent(context, request.getMethod());

        auto roles = params->getAll("role");
        if (!roles.empty())
            context->setCurrentRoles(roles);

        /// Settings can be overridden in the URL query.
        auto is_setting_like_parameter = [&] (const String & name)
        {
            /// Empty parameter appears when URL like ?&a=b or a=b&&c=d. Just skip them for user's convenience.
            if (name.empty())
                return false;

            /// Some parameters (database, default_format, everything used in the code above) do not
            /// belong to the Settings class.
            static const NameSet reserved_param_names{"user", "password", "quota_key", "stacktrace", "role", "query_id"};
            return !reserved_param_names.contains(name);
        };

        SettingsChanges settings_changes;
        for (const auto & [key, value] : *params)
        {
            if (is_setting_like_parameter(key))
            {
                /// This query parameter should be considered as a ClickHouse setting.
                settings_changes.push_back({key, value});
            }
        }

        context->checkSettingsConstraints(settings_changes, SettingSource::QUERY);
        context->applySettingsChanges(settings_changes);

        /// Set the query id supplied by the user, if any, and also update the OpenTelemetry fields.
        context->setCurrentQueryId(params->get("query_id", request.get("X-ClickHouse-Query-Id", "")));
    }

    void onException() override
    {
        // So that the next requests on the connection have to always start afresh in case of exceptions.
        request_credentials.reset();
    }

    const Settings & default_settings;
    std::unique_ptr<HTMLForm> params;
    std::unique_ptr<Session> session;
    std::unique_ptr<Credentials> request_credentials;
    ContextMutablePtr context;
};


/// Implementation of the remote-write protocol.
class PrometheusRequestHandler::RemoteWriteImpl : public ImplWithContext
{
public:
    using ImplWithContext::ImplWithContext;

    void beforeHandlingRequest(HTTPServerRequest & request) override
    {
        LOG_INFO(log(), "Handling remote write request from {}", request.get("User-Agent", ""));
        chassert(config().type == PrometheusRequestHandlerConfig::Type::RemoteWrite);
    }

    void handlingRequestWithContext([[maybe_unused]] HTTPServerRequest & request, [[maybe_unused]] HTTPServerResponse & response) override
    {
#if USE_PROMETHEUS_PROTOBUFS
        checkHTTPHeader(request, "Content-Type", "application/x-protobuf");
        checkHTTPHeader(request, "Content-Encoding", "snappy");

        ProtobufZeroCopyInputStreamFromReadBuffer zero_copy_input_stream{
            std::make_unique<SnappyReadBuffer>(wrapReadBufferReference(request.getStream()))};

        prometheus::WriteRequest write_request;
        if (!write_request.ParsePartialFromZeroCopyStream(&zero_copy_input_stream))
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cannot parse WriteRequest");

        auto table = DatabaseCatalog::instance().getTable(StorageID{config().time_series_table_name}, context);
        PrometheusRemoteWriteProtocol protocol{table, context};

        if (write_request.timeseries_size())
            protocol.writeTimeSeries(write_request.timeseries());

        if (write_request.metadata_size())
            protocol.writeMetricsMetadata(write_request.metadata());

        response.setStatusAndReason(Poco::Net::HTTPResponse::HTTPStatus::HTTP_NO_CONTENT, Poco::Net::HTTPResponse::HTTP_REASON_NO_CONTENT);
        response.setChunkedTransferEncoding(false);
        response.send();

#else
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "Prometheus remote write protocol is disabled");
#endif
    }
};

/// Implementation of the remote-read protocol.
class PrometheusRequestHandler::RemoteReadImpl : public ImplWithContext
{
public:
    using ImplWithContext::ImplWithContext;

    void beforeHandlingRequest(HTTPServerRequest & request) override
    {
        LOG_INFO(log(), "Handling remote read request from {}", request.get("User-Agent", ""));
        chassert(config().type == PrometheusRequestHandlerConfig::Type::RemoteRead);
    }

    void handlingRequestWithContext([[maybe_unused]] HTTPServerRequest & request, [[maybe_unused]] HTTPServerResponse & response) override
    {
#if USE_PROMETHEUS_PROTOBUFS
        checkHTTPHeader(request, "Content-Type", "application/x-protobuf");
        checkHTTPHeader(request, "Content-Encoding", "snappy");

        auto table = DatabaseCatalog::instance().getTable(StorageID{config().time_series_table_name}, context);
        PrometheusRemoteReadProtocol protocol{table, context};

        ProtobufZeroCopyInputStreamFromReadBuffer zero_copy_input_stream{
            std::make_unique<SnappyReadBuffer>(wrapReadBufferReference(request.getStream()))};

        prometheus::ReadRequest read_request;
        if (!read_request.ParseFromZeroCopyStream(&zero_copy_input_stream))
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cannot parse ReadRequest");

        prometheus::ReadResponse read_response;

        size_t num_queries = read_request.queries_size();
        for (size_t i = 0; i != num_queries; ++i)
        {
            const auto & query = read_request.queries(static_cast<int>(i));
            auto & new_query_result = *read_response.add_results();
            protocol.readTimeSeries(
                *new_query_result.mutable_timeseries(),
                query.start_timestamp_ms(),
                query.end_timestamp_ms(),
                query.matchers(),
                query.hints());
        }

#    if 0
    LOG_DEBUG(log, "ReadResponse = {}", read_response.DebugString());
#    endif

        response.setContentType("application/x-protobuf");
        response.set("Content-Encoding", "snappy");

        ProtobufZeroCopyOutputStreamFromWriteBuffer zero_copy_output_stream{std::make_unique<SnappyWriteBuffer>(getOutputStream(response))};
        read_response.SerializeToZeroCopyStream(&zero_copy_output_stream);
        zero_copy_output_stream.finalize();

#else
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "Prometheus remote read protocol is disabled");
#endif
    }
};


PrometheusRequestHandler::PrometheusRequestHandler(
    IServer & server_,
    const PrometheusRequestHandlerConfig & config_,
    const AsynchronousMetrics & async_metrics_,
    std::shared_ptr<PrometheusMetricsWriter> metrics_writer_,
    std::unordered_map<String, String> response_headers_)
    : server(server_)
    , config(config_)
    , async_metrics(async_metrics_)
    , metrics_writer(metrics_writer_)
    , log(getLogger("PrometheusRequestHandler"))
{
    response_headers = response_headers_;
    createImpl();
}

PrometheusRequestHandler::~PrometheusRequestHandler() = default;

void PrometheusRequestHandler::createImpl()
{
    switch (config.type)
    {
        case PrometheusRequestHandlerConfig::Type::ExposeMetrics:
        {
            impl = std::make_unique<ExposeMetricsImpl>(*this);
            return;
        }
        case PrometheusRequestHandlerConfig::Type::RemoteWrite:
        {
            impl = std::make_unique<RemoteWriteImpl>(*this);
            return;
        }
        case PrometheusRequestHandlerConfig::Type::RemoteRead:
        {
            impl = std::make_unique<RemoteReadImpl>(*this);
            return;
        }
    }
    UNREACHABLE();
}

void PrometheusRequestHandler::handleRequest(HTTPServerRequest & request, HTTPServerResponse & response, const ProfileEvents::Event & write_event_)
{
    setThreadName("PrometheusHndlr");
    applyHTTPResponseHeaders(response, response_headers);

    try
    {
        write_event = write_event_;
        http_method = request.getMethod();
        chassert(!write_buffer_from_response); /// Nothing is written to the response yet.

        /// Make keep-alive works.
        if (request.getVersion() == HTTPServerRequest::HTTP_1_1)
            response.setChunkedTransferEncoding(true);

        setResponseDefaultHeaders(response);

        impl->beforeHandlingRequest(request);
        impl->handleRequest(request, response);

        getOutputStream(response).finalize();
    }
    catch (...)
    {
        tryLogCurrentException(log);

        ExecutionStatus status = ExecutionStatus::fromCurrentException("", send_stacktrace);
        getOutputStream(response).cancelWithException(request, status.code, status.message, nullptr);

        tryCallOnException();
    }
}

WriteBufferFromHTTPServerResponse & PrometheusRequestHandler::getOutputStream(HTTPServerResponse & response)
{
    if (write_buffer_from_response)
        return *write_buffer_from_response;

    write_buffer_from_response = std::make_unique<WriteBufferFromHTTPServerResponse>(
        response, http_method == HTTPRequest::HTTP_HEAD, write_event);

    return *write_buffer_from_response;
}

void PrometheusRequestHandler::tryCallOnException()
{
    try
    {
        if (impl)
            impl->onException();
    }
    catch (...)
    {
        tryLogCurrentException(log, "onException");
    }
}

}
