/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/IPv4Address.h>
#include <AK/JsonArray.h>
#include <AK/LexicalPath.h>
#include <LibCore/Promise.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CacheStorageStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace {

constexpr size_t batch_size = 10;
constexpr auto batch_path = "/batch/"sv;
constexpr auto batch_body_prefix = "body-"sv;
constexpr auto cache_worker_path = "/cache-worker.js"sv;
constexpr auto cache_worker_script = R"js(
    (async () => {
        try {
            const cache = await caches.open("worker round trip");
            const response = await cache.match("/from-window");
            const fromWindow = response ? await response.text() : "missing";
            await cache.put("/from-worker", new Response(new Uint8Array([3, 0, 255])));
            postMessage(fromWindow);
        } catch (error) {
            postMessage(String(error));
        }
    })();
)js"sv;

class HeldBatchResponses {
public:
    size_t started() const { return m_started; }
    size_t active() const { return m_active; }
    size_t peak() const { return m_peak; }
    bool is_held(size_t index) const { return m_responses[index] != nullptr; }

    void hold(size_t index, Core::TCPSocket& socket)
    {
        VERIFY(!m_seen[index]);
        m_seen[index] = true;
        m_responses[index] = &socket;
        ++m_started;
        m_peak = max(m_peak, ++m_active);

        // End each response with its index, so headers and some body bytes arrive before its slot is free.
        auto response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: {}\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}", batch_body_prefix.length() + 1, batch_body_prefix);
        MUST(socket.set_blocking(true));
        MUST(socket.write_until_depleted(response.bytes()));
    }

    void release(size_t index)
    {
        auto* socket = exchange(m_responses[index], nullptr);
        VERIFY(socket);
        --m_active;
        auto suffix = ByteString::formatted("{}", index);
        MUST(socket->write_until_depleted(suffix.bytes()));
        socket->close();
    }

private:
    Array<Core::TCPSocket*, batch_size> m_responses {};
    Array<bool, batch_size> m_seen {};
    size_t m_started { 0 };
    size_t m_active { 0 };
    size_t m_peak { 0 };
};

class PageServer {
public:
    explicit PageServer(HeldBatchResponses* batch = nullptr)
        : m_server(MUST(Core::TCPServer::try_create()))
        , m_batch(batch)
    {
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            m_connections.append(make<Connection>(move(socket)));
            auto& connection = *m_connections.last();
            connection.socket->on_ready_to_read = [this, &connection] { respond(connection); };
        };
    }

    URL::URL url(StringView path = "/"sv) const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}{}", *m_server->local_port(), path)).release_value();
    }

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;

        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer request {};
    };

    void respond(Connection& connection)
    {
        Array<u8, 4096> buffer;
        auto result = connection.socket->read_some(buffer);
        if (!result.is_error() && !result.value().is_empty()) {
            connection.request.append(result.value());
            StringView request { connection.request.bytes() };
            if (!request.contains("\r\n\r\n"sv) && connection.request.size() <= 16 * KiB)
                return;

            auto request_line = request.split_view("\r\n"sv).first().split_view(' ');
            auto path = request_line.size() >= 2 && request_line[0] == "GET"sv ? request_line[1] : ""sv;
            if (m_batch && path.starts_with(batch_path)) {
                auto index = path.substring_view(batch_path.length()).to_number<size_t>();
                if (index.has_value() && *index < batch_size) {
                    connection.socket->on_ready_to_read = nullptr;
                    m_batch->hold(*index, *connection.socket);
                    return;
                }
            }

            auto is_worker = path == cache_worker_path;
            auto body = is_worker ? cache_worker_script : "<!doctype html><title>Cache Storage test</title>"sv;
            auto response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n{}", is_worker ? "text/javascript"sv : "text/html"sv, body.length(), body);
            MUST(connection.socket->set_blocking(true));
            MUST(connection.socket->write_until_depleted(response.bytes()));
        }

        connection.socket->on_ready_to_read = nullptr;
        connection.socket->close();
    }

    NonnullRefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Connection>> m_connections;
    HeldBatchResponses* m_batch { nullptr };
};

static bool wait_until(Function<bool()> condition)
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return condition() || timed_out; });
    timer->stop();
    return condition();
}

static ErrorOr<void> wait_for_load(WebView::HeadlessWebView& view, URL::URL const& expected, Function<void()> start)
{
    bool loaded = false;
    view.on_load_finish = [&](auto const& url) { loaded |= url.equals(expected); };
    start();
    auto finished = wait_until([&] { return loaded; });
    view.on_load_finish = nullptr;
    if (!finished)
        return Error::from_string_literal("Page did not finish loading");
    return {};
}

static ErrorOr<ByteString> run_cache_script(WebView::HeadlessWebView& view, StringView step, StringView script)
{
    auto prefix = ByteString::formatted("{}: ", step);
    Optional<ByteString> result;
    view.on_title_change = [&](auto const& title) {
        auto text = title.to_byte_string();
        if (text.starts_with(prefix))
            result = text.substring(prefix.length());
    };
    view.run_javascript(MUST(String::formatted(R"js(
        (async () => {{
            const prefix = "{}";
            try {{
                document.title = prefix + await (async () => {{ {} }})();
            }} catch (error) {{
                document.title = prefix + error;
            }}
        }})();
    )js",
        prefix, script)));
    auto finished = wait_until([&] { return result.has_value(); });
    view.on_title_change = nullptr;
    if (!finished) {
        warnln("No result from Cache Storage test step {}", step);
        return Error::from_string_literal("Cache Storage script did not finish");
    }
    return result.release_value();
}

}

TEST_CASE(cache_storage_shares_between_renderers_and_isolates_origins_and_sessions)
{
    Test::HeadlessTestEnvironment environment { "TestCacheStorageIntegration"sv };
    auto app = TRY_OR_FAIL(environment.create_application());

    PageServer server;
    PageServer different_origin;
    auto url = server.url();
    auto other_url = different_origin.url();
    VERIFY(!url.equals(other_url));

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto open = [&](URL::URL const& target, WebView::IsPrivate is_private = WebView::IsPrivate::No) -> ErrorOr<NonnullOwnPtr<WebView::HeadlessWebView>> {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, is_private);
        TRY(wait_for_load(*view, target, [&] { view->load_from_browser_ui(target); }));
        return view;
    };

    auto writer = TRY_OR_FAIL(open(url));
    auto reader = TRY_OR_FAIL(open(url));
    auto private_reader = TRY_OR_FAIL(open(url, WebView::IsPrivate::Yes));
    auto other_reader = TRY_OR_FAIL(open(other_url));

    auto& regular_session = writer->session();
    VERIFY(writer->page().client().pid() != reader->page().client().pid());
    VERIFY(&writer->page().client().session() == &regular_session);
    VERIFY(&reader->page().client().session() == &regular_session);
    VERIFY(&other_reader->page().client().session() == &regular_session);
    VERIFY(&private_reader->session() != &regular_session);
    VERIFY(&private_reader->page().client().session() == &private_reader->session());
    VERIFY(regular_session.install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_memory_backed())));
    VERIFY(private_reader->session().install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_memory_backed())));

    auto const normal_body = R"js(
        const response = await caches.match("/entry", { cacheName: "shared" });
        return response ? await response.text() : "missing";
    )js"sv;

    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*writer, "seed"sv, R"js(
        window.oldCache = await caches.open("shared");
        const request = new Request("/entry", { referrer: "/referrer?source=cache#saved" });
        await window.oldCache.put(request, new Response("regular body"));
        return await (await window.oldCache.match("/entry")).text();
    )js"sv)),
        "regular body"sv);

    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*reader, "separate renderer"sv, R"js(
        window.beforeReload = true;
        const cache = await caches.open("shared");
        const [request] = await cache.keys();
        const expectedReferrer = new URL("/referrer?source=cache#saved", location.href).href;
        const response = await cache.match("/entry");
        return `${response ? await response.text() : "missing"}; ${request?.referrer === expectedReferrer}`;
    )js"sv)),
        "regular body; true"sv);

    TRY_OR_FAIL(wait_for_load(*reader, url, [&] { reader->reload(WebView::BypassCache::Yes); }));
    VERIFY(&reader->page().client().session() == &regular_session);
    VERIFY(writer->page().client().pid() != reader->page().client().pid());
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*reader, "reload"sv, R"js(
        const response = await caches.match("/entry", { cacheName: "shared" });
        return `${!window.beforeReload}; ${response ? await response.text() : "missing"}`;
    )js"sv)),
        "true; regular body"sv);

    // Both origins are on the same host. Check cache names as well as entries, since a relative request changes origin.
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*other_reader, "different origin"sv, R"js(
        return `${await caches.has("shared")}; ${(await caches.keys()).length}`;
    )js"sv)),
        "false; 0"sv);

    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*private_reader, "private origin"sv, R"js(
        const initiallyAbsent = !(await caches.has("shared"));
        const cache = await caches.open("shared");
        await cache.put("/entry", new Response("private body"));
        return `${initiallyAbsent}; ${await (await cache.match("/entry")).text()}`;
    )js"sv)),
        "true; private body"sv);
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*writer, "regular after private write"sv, normal_body)), "regular body"sv);

    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*reader, "delete and reopen"sv, R"js(
        const deleted = await caches.delete("shared");
        const absent = !(await caches.has("shared"));
        const newCache = await caches.open("shared");
        const empty = (await newCache.match("/entry")) === undefined;
        await newCache.put("/entry", new Response("new body"));
        return `${deleted}; ${absent}; ${empty}`;
    )js"sv)),
        "true; true; true"sv);

    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*writer, "old handle"sv, R"js(
        const original = await (await window.oldCache.match("/entry")).text();
        await window.oldCache.put("/entry", new Response("detached body"));
        const replacement = await caches.match("/entry", { cacheName: "shared" });
        return `${original}; ${replacement ? await replacement.text() : "missing"}`;
    )js"sv)),
        "regular body; new body"sv);

    // Save an older cache in the same session, then clear newer site data through the browser.
    regular_session.cache_storage_store()->set_clock_for_testing([] { return UnixDateTime::from_seconds_since_epoch(100); });
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*writer, "older cache"sv, R"js(
        const cache = await caches.open("retain");
        await cache.put("/entry", new Response("retained body"));
        return await (await cache.match("/entry")).text();
    )js"sv)),
        "retained body"sv);
    regular_session.cache_storage_store()->set_clock_for_testing({});

    auto const cutoff = UnixDateTime::from_seconds_since_epoch(200);
    auto regular_sizes = TRY_OR_FAIL(regular_session.cache_storage_store()->estimate_accessed_since(cutoff));
    VERIFY(regular_sizes.since_requested_time < regular_sizes.total);
    auto reported_sizes = TRY_OR_FAIL(app->estimate_browsing_data_size_accessed_since(cutoff)->await());
    EXPECT_EQ(reported_sizes.site_data_size_since_requested_time, regular_sizes.since_requested_time);
    EXPECT_EQ(reported_sizes.total_site_data_size, regular_sizes.total);

    TRY_OR_FAIL(app->clear_browsing_data({
                                             .since = cutoff,
                                             .delete_site_data = WebView::Application::ClearBrowsingDataOptions::Delete::Yes,
                                         })
            ->await());
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*writer, "regular after clear"sv, R"js(
        let rejected = false;
        try {
            await window.oldCache.match("/entry");
        } catch (error) {
            rejected = error instanceof DOMException;
        }
        const older = await caches.match("/entry", { cacheName: "retain" });
        return `${rejected}; ${await caches.has("shared")}; ${older ? await older.text() : "missing"}`;
    )js"sv)),
        "true; false; retained body"sv);
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*private_reader, "private after clear"sv, normal_body)), "missing"sv);
}

TEST_CASE(private_cache_add_all_fills_eight_slots_and_publishes_in_input_order)
{
    Test::HeadlessTestEnvironment environment { "TestPrivateCacheAddAllWindow"sv };
    auto app = TRY_OR_FAIL(environment.create_application());

    HeldBatchResponses batch;
    PageServer page_server;
    // Keep each origin below RequestServer's six connections per host and port.
    PageServer first_origin { &batch };
    PageServer second_origin { &batch };
    auto url = page_server.url();

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto writer = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);
    TRY_OR_FAIL(wait_for_load(*writer, url, [&] { writer->load_from_browser_ui(url); }));
    auto observer = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);
    TRY_OR_FAIL(wait_for_load(*observer, url, [&] { observer->load_from_browser_ui(url); }));

    auto& session = writer->session();
    VERIFY(&observer->session() == &session);
    VERIFY(writer->page().client().pid() != observer->page().client().pid());
    VERIFY(session.install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_memory_backed())));

    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*writer, "seed"sv, R"js(
        window.batchCache = await caches.open("bounded batch");
        await window.batchCache.put("/sentinel", new Response("retained"));
        return "seeded";
    )js"sv)),
        "seeded"sv);
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*observer, "observe"sv, R"js(
        window.batchCache = await caches.open("bounded batch");
        return (await window.batchCache.keys()).length;
    )js"sv)),
        "1"sv);

    JsonArray urls;
    for (size_t index = 0; index < batch_size; ++index) {
        auto& origin = index % 2 == 0 ? first_origin : second_origin;
        auto path = ByteString::formatted("{}{}", batch_path, index);
        urls.must_append(origin.url(path).serialize());
    }
    auto start = TRY_OR_FAIL(String::formatted("window.batchResult = window.batchCache.addAll({}).then(() => 'complete', error => String(error)); return 'started';", urls.serialized()));
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*writer, "start batch"sv, start)), "started"sv);

    auto const check_unpublished = R"js(
        const [first, ...others] = await window.batchCache.keys();
        const response = first && await window.batchCache.match(first);
        return `${others.length}; ${response ? await response.text() : "missing"}`;
    )js"sv;
    VERIFY(wait_until([&] { return batch.started() >= 8; }));
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*observer, "while initial slots are held"sv, check_unpublished)), "0; retained"sv);
    EXPECT_EQ(batch.active(), 8u);

    // Complete two later responses while the first is still waiting. Each completion should admit one more request.
    batch.release(1);
    VERIFY(wait_until([&] { return batch.started() >= 9; }));
    EXPECT_EQ(batch.active(), 8u);
    batch.release(3);
    VERIFY(wait_until([&] { return batch.started() == batch_size; }));
    EXPECT_EQ(batch.active(), 8u);
    EXPECT(batch.is_held(0));
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*observer, "after out-of-order responses"sv, check_unpublished)), "0; retained"sv);

    for (size_t index = batch_size; index > 0; --index) {
        if (batch.is_held(index - 1))
            batch.release(index - 1);
    }
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*writer, "finish batch"sv, "return await window.batchResult;"sv)), "complete"sv);
    EXPECT_EQ(batch.peak(), 8u);
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*observer, "read complete batch"sv, R"js(
        const requests = await window.batchCache.keys();
        const paths = requests.map(request => new URL(request.url).pathname).join(",");
        const bodies = await Promise.all(requests.map(async request => (await window.batchCache.match(request)).text()));
        return `${paths}; ${bodies.join(",")}`;
    )js"sv)),
        "/sentinel,/batch/0,/batch/1,/batch/2,/batch/3,/batch/4,/batch/5,/batch/6,/batch/7,/batch/8,/batch/9; retained,body-0,body-1,body-2,body-3,body-4,body-5,body-6,body-7,body-8,body-9"sv);
}

#if defined(AK_OS_LINUX)
TEST_CASE(ordinary_cache_storage_reopens_in_a_fresh_application_and_session)
{
    Test::HeadlessTestEnvironment environment { "TestCacheStorageReopen"sv };
    Test::TemporaryDirectory disk { "TestCacheStorageReopenStore"sv };
    PageServer server;
    auto url = server.url();

    auto open = [&](WebView::IsPrivate is_private) -> ErrorOr<NonnullOwnPtr<WebView::HeadlessWebView>> {
        auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
        auto theme = TRY(Gfx::load_system_theme(theme_path.string()));
        auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 }, is_private);
        TRY(wait_for_load(*view, url, [&] { view->load_from_browser_ui(url); }));
        return view;
    };

    u64 ordinary_session_id;
    u64 private_session_id;
    {
        auto app = TRY_OR_FAIL(environment.create_application());
        auto& ordinary_session = app->default_session();
        ordinary_session_id = ordinary_session.id();
        VERIFY(ordinary_session.install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_file_backed(disk.path().string()))));
        auto ordinary = TRY_OR_FAIL(open(WebView::IsPrivate::No));
        EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*ordinary, "save ordinary bytes"sv, R"js(
            const cache = await caches.open("ordinary marker");
            await cache.put("/entry", new Response(new Uint8Array([0, 128, 255])));
            return (await cache.keys()).length;
        )js"sv)),
            "1"sv);

        auto private_view = TRY_OR_FAIL(open(WebView::IsPrivate::Yes));
        private_session_id = private_view->session().id();
        VERIFY(private_view->session().install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_memory_backed())));
        EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*private_view, "save private marker"sv, R"js(
            const cache = await caches.open("private marker");
            await cache.put("/entry", new Response("private only"));
            return await (await cache.match("/entry")).text();
        )js"sv)),
            "private only"sv);
    }

    // Keep the loopback origin and disk directory, but recreate the application in this test process.
    VERIFY(!WebView::Application::is_initialized());

    auto app = TRY_OR_FAIL(environment.create_application());
    auto& ordinary_session = app->default_session();
    VERIFY(ordinary_session.id() != ordinary_session_id);
    VERIFY(ordinary_session.install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_file_backed(disk.path().string()))));
    auto ordinary = TRY_OR_FAIL(open(WebView::IsPrivate::No));
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*ordinary, "reopen ordinary store"sv, R"js(
        const response = await caches.match("/entry", { cacheName: "ordinary marker" });
        const body = response ? [...new Uint8Array(await response.arrayBuffer())].join(",") : "missing";
        return `${body}; ${await caches.has("private marker")}`;
    )js"sv)),
        "0,128,255; false"sv);

    auto private_view = TRY_OR_FAIL(open(WebView::IsPrivate::Yes));
    VERIFY(private_view->session().id() != private_session_id);
    VERIFY(private_view->session().install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_memory_backed())));
    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*private_view, "fresh private store"sv, "return (await caches.keys()).length;"sv)), "0"sv);
}
#endif

TEST_CASE(removing_the_owner_settles_shared_cache_operations)
{
    Test::HeadlessTestEnvironment environment { "TestCacheStorageOwnerRemoval"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    VERIFY(app->default_session().install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_memory_backed())));

    PageServer server;
    auto url = server.url();
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    TRY_OR_FAIL(wait_for_load(*view, url, [&] { view->load_from_browser_ui(url); }));

    // Operations on a cache whose owning frame goes away, started before and after its removal, must not stay pending.
    auto result = TRY_OR_FAIL(run_cache_script(*view, "owner removal"sv, R"js(
        const frame = document.createElement("iframe");
        frame.src = "/frame";
        await new Promise(resolve => { frame.onload = resolve; document.body.append(frame); });
        const FrameResponse = frame.contentWindow.Response;
        const entry = new URL("/entry", location.href).href;
        const other = new URL("/other", location.href).href;
        const cache = await frame.contentWindow.caches.open("owner removal");
        await cache.put(entry, new FrameResponse("body"));

        const start = () => [cache.keys(), cache.matchAll(), cache.put(other, new FrameResponse("other")), cache.delete(entry)];
        const settle = promises => {
            const timeout = new Promise(resolve => setTimeout(() => resolve("pending"), 2000));
            return Promise.all(promises.map(promise => Promise.race([promise.then(() => "resolved", error => error.name), timeout])));
        };
        const before = start();
        frame.remove();
        const settledBefore = await settle(before);
        await new Promise(resolve => setTimeout(resolve, 100));
        const settledAfter = await settle(start());
        return [...settledBefore, ...settledAfter].join(",");
    )js"sv));

    auto outcomes = result.split(',');
    EXPECT_EQ(outcomes.size(), 8u);
    for (auto const& outcome : outcomes) {
        if (outcome != "resolved"sv && outcome != "AbortError"sv)
            FAIL(ByteString::formatted("Unexpected outcome in {}", result));
    }
}

TEST_CASE(operations_called_from_a_removed_frame_settle_consistently)
{
    Test::HeadlessTestEnvironment environment { "TestCacheStorageCrossRealm"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    VERIFY(app->default_session().install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_memory_backed())));

    PageServer server;
    auto url = server.url();
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    TRY_OR_FAIL(wait_for_load(*view, url, [&] { view->load_from_browser_ui(url); }));

    // The page's own caches, called with the methods of a frame, give their promises to the frame's realm. Once the
    // frame is removed, each promise must settle: resolved if the operation was applied, otherwise with an AbortError.
    auto result = TRY_OR_FAIL(run_cache_script(*view, "cross realm"sv, R"js(
        const frame = document.createElement("iframe");
        frame.src = "/frame";
        await new Promise(resolve => { frame.onload = resolve; document.body.append(frame); });
        const { Cache: FrameCache, CacheStorage: FrameCacheStorage, Response: FrameResponse } = frame.contentWindow;
        const cache = await caches.open("cross realm");
        const entry = name => new URL(`/${name}`, location.href).href;
        const outcome = promise => Promise.race([
            promise.then(() => "resolved", error => error.name),
            new Promise(resolve => setTimeout(() => resolve("pending"), 2000)),
        ]);
        const start = name => [
            FrameCache.prototype.put.call(cache, entry(name), new FrameResponse(name)),
            FrameCacheStorage.prototype.open.call(caches, `opened ${name}`),
            FrameCacheStorage.prototype.has.call(caches, "cross realm"),
        ];

        const before = start("before");
        frame.remove();
        const settledBefore = await Promise.all(before.map(outcome));
        await new Promise(resolve => setTimeout(resolve, 100));
        const settledAfter = await Promise.all(start("after").map(outcome));
        const stored = [(await cache.match(entry("before"))) !== undefined, (await cache.match(entry("after"))) !== undefined];
        return `${settledBefore.join(",")};${settledAfter.join(",")};${stored.join(",")}`;
    )js"sv));
    outln("Outcomes before removal; after removal; entries stored: {}", result);

    auto parts = result.split(';');
    VERIFY(parts.size() == 3);
    auto before = parts[0].split(',');
    auto after = parts[1].split(',');
    auto stored = parts[2].split(',');
    VERIFY(before.size() == 3 && after.size() == 3 && stored.size() == 2);
    for (auto const* outcomes : { &before, &after }) {
        for (auto const& outcome : *outcomes) {
            if (outcome != "resolved"sv && outcome != "AbortError"sv)
                FAIL(ByteString::formatted("Unexpected outcome in {}", result));
        }
    }
    // A put resolves exactly when the Hub stored its entry.
    EXPECT_EQ(before[0] == "resolved"sv, stored[0] == "true"sv);
    EXPECT_EQ(after[0] == "resolved"sv, stored[1] == "true"sv);
}

TEST_CASE(same_origin_dedicated_worker_shares_cache_storage_with_its_window)
{
    Test::HeadlessTestEnvironment environment { "TestCacheStorageDedicatedWorker"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    VERIFY(app->default_session().install_cache_storage_store(TRY_OR_FAIL(WebView::CacheStorageStore::create_memory_backed())));

    PageServer server;
    auto url = server.url();
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    TRY_OR_FAIL(wait_for_load(*view, url, [&] { view->load_from_browser_ui(url); }));

    EXPECT_EQ(TRY_OR_FAIL(run_cache_script(*view, "worker round trip"sv, R"js(
        const cache = await caches.open("worker round trip");
        await cache.put("/from-window", new Response("window text"));
        const worker = new Worker("/cache-worker.js");
        try {
            const workerRead = await new Promise((resolve, reject) => {
                worker.onmessage = event => resolve(event.data);
                worker.onerror = event => reject(new Error(event.message || "worker failed"));
            });
            const response = await cache.match("/from-worker");
            const workerBytes = response ? [...new Uint8Array(await response.arrayBuffer())].join(",") : "missing";
            return `${workerRead}; ${workerBytes}`;
        } finally {
            worker.terminate();
        }
    )js"sv)),
        "window text; 3,0,255"sv);
}
