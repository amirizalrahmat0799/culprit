#include <filesystem>
#include <fstream>

#include "ai/workspace.hpp"
#include "check.hpp"

using culprit::ai::Workspace;
namespace fs = std::filesystem;

namespace {
void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// a small project: code, config with a password, secrets, dependencies, and a git-ignored file
fs::path make_project() {
    fs::path root = fs::temp_directory_path() / ("culprit-ws-test-" + std::to_string(std::rand()));
    fs::remove_all(root);
    write(root / "src/main/java/com/example/kedai/order/OrderService.java",
          "package com.example.kedai.order;\n\npublic class OrderService {\n    void place(Order o) {\n"
          "        String email = o.getCustomer().getEmail();\n    }\n}\n");
    write(root / "src/test/java/com/example/kedai/order/OrderServiceTest.java", "class OrderServiceTest {}\n");
    write(root / "src/main/resources/application.properties",
          "server.port=8080\nspring.datasource.password=hunter2\nspring.datasource.username=kedai\napi.key=${API_KEY}\n");
    write(root / "web/app.js", "const token = \"abc123\";\nconst client = new Client({ apiKey: 'sk-proj-ABCDEFGHIJKLMNOP1234' });\n");
    write(root / ".env", "DB_PASSWORD=hunter2\n");
    write(root / ".env.example", "DB_PASSWORD=\n");
    write(root / "k8s/secrets.yaml", "kind: Secret\n");
    write(root / "certs/server.key", "-----BEGIN PRIVATE KEY-----\n");
    write(root / "node_modules/lib/index.js", "OrderService everywhere\n");
    write(root / "local.properties", "spring.datasource.password=local\n");
    write(root / ".gitignore", "local.properties\n");
    std::string out;
    if (culprit::ai::run_program({"git", "-C", root.string(), "init", "-q"}, out) == 0) {
        culprit::ai::run_program({"git", "-C", root.string(), "-c", "user.email=t@t", "-c", "user.name=t", "add", "."}, out);
        culprit::ai::run_program({"git", "-C", root.string(), "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "-m", "first"}, out);
    }
    return root;
}
}  // namespace

TEST("ai workspace: paths from stack traces and containers are found in the project") {
    fs::path root = make_project();
    Workspace ws(root.string());
    CHECK(ws.resolve("OrderService.java") == std::optional<std::string>("src/main/java/com/example/kedai/order/OrderService.java"));
    CHECK(ws.resolve("/app/src/main/resources/application.properties") ==
          std::optional<std::string>("src/main/resources/application.properties"));
    CHECK(ws.resolve("kedai/order/OrderService.java").has_value());
    CHECK(ws.resolve((root / "web/app.js").string()) == std::optional<std::string>("web/app.js"));
    std::string why;
    CHECK(!ws.resolve("Missing.java", &why));
    CHECK(why.find("no file") != std::string::npos);
    fs::remove_all(root);
}

TEST("ai workspace: nothing outside the project, and no secrets") {
    fs::path root = make_project();
    Workspace ws(root.string());
    std::string why;
    CHECK(!ws.resolve("../../../../etc/passwd", &why));
    CHECK(!ws.resolve("/etc/passwd", &why));
    CHECK(ws.read_file("../../etc/passwd", 1, 5).rfind("error:", 0) == 0);
    CHECK(ws.read_file(".env", 1, 5).find("may hold secrets") != std::string::npos);
    CHECK(ws.read_file(".env.example", 1, 5).find("DB_PASSWORD") != std::string::npos);
    CHECK(ws.read_file("k8s/secrets.yaml", 1, 5).rfind("error:", 0) == 0);
    CHECK(ws.read_file("certs/server.key", 1, 5).rfind("error:", 0) == 0);
    CHECK(ws.read_file(".git/config", 1, 5).rfind("error:", 0) == 0);
    std::string out;
    if (culprit::ai::run_program({"git", "--version"}, out) == 0) {
        CHECK(ws.read_file("local.properties", 1, 5).find("ignored by git") != std::string::npos);
    }
    // a symbolic link pointing outside
    std::error_code ec;
    fs::create_symlink("/etc", root / "etc-link", ec);
    if (!ec) CHECK(ws.read_file("etc-link/hostname", 1, 5).rfind("error:", 0) == 0);
    fs::remove_all(root);
}

TEST("ai workspace: secret values are redacted in what is read") {
    fs::path root = make_project();
    Workspace ws(root.string());
    std::string props = ws.read_file("application.properties", 1, 10);
    CHECK(props.find("server.port=8080") != std::string::npos);
    CHECK(props.find("hunter2") == std::string::npos);
    CHECK(props.find("spring.datasource.password=[redacted]") != std::string::npos);
    CHECK(props.find("api.key=${API_KEY}") != std::string::npos);  // a reference, not a secret
    CHECK(props.find("username=kedai") != std::string::npos);
    std::string js = ws.read_file("web/app.js", 1, 10);
    CHECK(js.find("abc123") == std::string::npos);
    CHECK(js.find("sk-proj") == std::string::npos);
    CHECK_EQ(Workspace::redact("String password = request.getPassword();", false),
             std::string("String password = request.getPassword();"));
    CHECK_EQ(Workspace::redact("-----BEGIN RSA PRIVATE KEY-----", false), std::string("[redacted private key]"));
    fs::remove_all(root);
}

TEST("ai workspace: read ranges, search, list and find") {
    fs::path root = make_project();
    Workspace ws(root.string());
    std::string r = ws.read_file("OrderService.java", 4, 5);
    CHECK(r.find("4:     void place(Order o) {") != std::string::npos);
    CHECK(r.find("5:         String email") != std::string::npos);
    CHECK(r.find("3: ") == std::string::npos);
    CHECK(r.find("more lines") != std::string::npos);
    CHECK_EQ(ws.files_read().size(), 1u);

    std::string s = ws.search("orderservice", "");
    CHECK(s.find("OrderService.java:3:") != std::string::npos);
    CHECK(s.find("node_modules") == std::string::npos);
    CHECK(ws.search("hunter2", "").find(".env") == std::string::npos);
    CHECK(ws.search("hunter2", "").find("local.properties") == std::string::npos || !fs::exists(root / ".git"));

    std::string l = ws.list_dir(".");
    CHECK(l.find("src/") != std::string::npos);
    CHECK(l.find(".env  (not readable") != std::string::npos);
    CHECK(l.find("node_modules/  (skipped") != std::string::npos);
    CHECK(ws.find_files("OrderService").find("OrderServiceTest.java") != std::string::npos);
    CHECK(ws.overview().find("Top level:") != std::string::npos);
    fs::remove_all(root);
}

TEST("ai workspace: git history shows uncommitted changes") {
    fs::path root = make_project();
    if (!fs::exists(root / ".git")) {
        fs::remove_all(root);
        return;
    }
    write(root / "src/main/java/com/example/kedai/order/OrderService.java", "package com.example.kedai.order;\n// changed\n");
    Workspace ws(root.string());
    std::string h = ws.git_history("OrderService.java");
    CHECK(h.find("Uncommitted changes") != std::string::npos);
    CHECK(h.find("+// changed") != std::string::npos);
    CHECK(h.find("first") != std::string::npos);
    fs::remove_all(root);
}
