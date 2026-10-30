#include "check.hpp"
#include "core/stack.hpp"

using namespace culprit;

TEST("Java frames and own code") {
    auto frames = parse_stack(
        "java.lang.NullPointerException: customer is null\n"
        "\tat com.example.kedai.order.OrderService.sendConfirmation(OrderService.java:87)\n"
        "\tat java.base/java.lang.reflect.Method.invoke(Method.java:580)\n"
        "\tat org.springframework.web.servlet.FrameworkServlet.service(FrameworkServlet.java:885)\n"
        "\tat com.example.kedai.report.SalesReportRepository$$SpringCGLIB$$0.dailyTotals(<generated>)\n"
        "Caused by: java.sql.SQLException: boom\n"
        "\t... 14 common frames omitted");
    CHECK_EQ(frames.size(), 7u);
    CHECK(frames[0].kind == Frame::Kind::Header);
    CHECK(frames[1].own);
    CHECK_EQ(frames[1].file, "OrderService.java");
    CHECK_EQ(frames[1].line, 87);
    CHECK_EQ(frames[1].function, "com.example.kedai.order.OrderService.sendConfirmation");
    CHECK(!frames[2].own);
    CHECK_EQ(frames[2].function, "java.lang.reflect.Method.invoke");
    CHECK(!frames[3].own);
    CHECK(!frames[4].own);  // a generated proxy
    CHECK(frames[5].kind == Frame::Kind::CausedBy);
    auto first = first_own_frame(frames);
    CHECK(first && first->line == 87);
}

TEST("Python, Node and Go frames") {
    auto py = parse_stack("Traceback (most recent call last):\n"
                          "  File \"/app/app/api.py\", line 42, in create_todo\n"
                          "    todo = store.create(title)\n"
                          "  File \"/usr/local/lib/python3.13/site-packages/sqlalchemy/engine/base.py\", line 1418, in execute\n"
                          "ValueError: bad");
    auto first = first_own_frame(py);
    CHECK(first && first->file == "/app/app/api.py" && first->line == 42 && first->function == "create_todo");
    CHECK(!py[3].own);

    auto node = parse_stack("TypeError: x\n    at createTodo (/app/src/app.ts:31:27)\n"
                            "    at Layer.handle (/app/node_modules/express/lib/router/layer.js:95:5)\n"
                            "    at node:internal/process/task_queues:105:5");
    CHECK(node[1].own && node[1].file == "/app/src/app.ts" && node[1].line == 31);
    CHECK(!node[2].own);
    CHECK(!node[3].own);

    auto go = parse_stack("panic: nil\n\ngoroutine 42 [running]:\nmain.(*PostgresStore).SetDone(0x0)\n"
                          "\t/app/postgres.go:118 +0x2e\nnet/http.HandlerFunc.ServeHTTP(0x0)\n"
                          "\t/usr/local/go/src/net/http/server.go:2294 +0x29");
    auto g = first_own_frame(go);
    CHECK(g && g->file == "/app/postgres.go" && g->line == 118 && g->function == "main.(*PostgresStore).SetDone");
}

TEST("absurd line numbers are treated as unknown") {
    auto f = parse_stack("java.lang.IllegalStateException: x\n\tat com.example.A.b(A.java:99999999999)\n\tat com.example.A.c(A.java:12)");
    CHECK_EQ(f[1].line, 0);
    CHECK_EQ(f[2].line, 12);
}
