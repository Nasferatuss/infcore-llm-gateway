// infcore — лицензия MIT (см. LICENSE). Юнит-тесты слоя gateway (без движка/сети).
// Минимальный assert-харнесс (без внешних зависимостей, offline).
#include <cstdio>
#include <fstream>
#include <string>

#include "gateway/config.hpp"
#include "gateway/json_schema.hpp"
#include "runtime/backend_supervisor.h"
#include "security/authn/authn.h"
#include "security/rbac/rbac.h"

using nlohmann::json;

static int g_fail = 0;
static int g_total = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_total;                                                       \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

// --- RBAC (default-deny) ------------------------------------------------------
static void test_rbac() {
    using namespace infcore;
    Authorizer a;
    a.set_enabled(true);
    Role admin; admin.name = "admin"; admin.allow_models = {"*"}; admin.allow_endpoints = {"*"};
    Role emb; emb.name = "emb"; emb.allow_models = {"bge"}; emb.allow_endpoints = {"/v1/embeddings"};
    a.add_role(admin);
    a.add_role(emb);
    std::string r;

    CHECK(a.allow("admin", "/v1/chat/completions", "anything", r));   // wildcard
    CHECK(a.allow("emb", "/v1/embeddings", "bge", r));                // exact
    CHECK(!a.allow("emb", "/v1/chat/completions", "bge", r));         // endpoint denied
    CHECK(!a.allow("emb", "/v1/embeddings", "other", r));             // model denied
    CHECK(!a.allow("ghost", "/v1/embeddings", "bge", r));             // unknown role -> deny
    CHECK(a.model_allowed("emb", "bge"));
    CHECK(!a.model_allowed("emb", "other"));
    CHECK(!a.model_allowed("ghost", "bge"));

    Authorizer off;
    off.set_enabled(false);                                           // RBAC выкл -> всё разрешено
    CHECK(off.allow("anyrole", "/whatever", "anymodel", r));
    CHECK(off.model_allowed("anyrole", "anymodel"));
}

// --- authn + parse_bearer -----------------------------------------------------
static void test_authn() {
    using namespace infcore;
    Authenticator au;
    au.add_key("key-admin", Principal{"alice", "admin"});
    au.add_key("key-emb", Principal{"svc", "emb"});
    Principal out;
    CHECK(au.verify("key-admin", out) && out.subject == "alice" && out.role == "admin");
    CHECK(au.verify("key-emb", out) && out.role == "emb");
    CHECK(!au.verify("wrong", out));
    CHECK(!au.verify("", out));
    CHECK(!au.verify("key-admin-extra", out));   // не префиксное совпадение

    CHECK(parse_bearer("Bearer abc") == "abc");
    CHECK(parse_bearer("bearer abc") == "abc");   // регистронезависимо
    CHECK(parse_bearer("BEARER abc") == "abc");
    CHECK(parse_bearer("Bearer ") == "");
    CHECK(parse_bearer("Bearer") == "");
    CHECK(parse_bearer("Basic abc") == "");
    CHECK(parse_bearer("") == "");
}

// --- json-schema валидатор ----------------------------------------------------
static const char* kSchema = R"({
  "type": "object", "additionalProperties": false,
  "required": ["name", "port"],
  "properties": {
    "name": { "type": "string", "pattern": "^[a-z]+$" },
    "port": { "type": "integer", "minimum": 1, "maximum": 65535 },
    "mode": { "enum": ["a", "b"] },
    "tags": { "type": "array", "minItems": 1, "items": { "type": "string" } }
  }
})";

static void test_json_schema() {
    using namespace infcore;
    json schema = json::parse(kSchema);
    auto ok = [&](const char* js) { return json_schema_validate(json::parse(js), schema).empty(); };

    CHECK(ok(R"({"name":"abc","port":8080})"));
    CHECK(ok(R"({"name":"abc","port":1,"mode":"a","tags":["x"]})"));
    CHECK(!ok(R"({"name":"abc"})"));                       // required port
    CHECK(!ok(R"({"name":"abc","port":8080,"extra":1})")); // additionalProperties
    CHECK(!ok(R"({"name":"abc","port":"80"})"));           // type
    CHECK(!ok(R"({"name":"ABC","port":8080})"));           // pattern
    CHECK(!ok(R"({"name":"abc","port":0})"));              // minimum
    CHECK(!ok(R"({"name":"abc","port":70000})"));          // maximum
    CHECK(!ok(R"({"name":"abc","port":80,"mode":"c"})"));  // enum
    CHECK(!ok(R"({"name":"abc","port":80,"tags":[]})"));   // minItems
}

// --- load_config error paths --------------------------------------------------
static std::string write_tmp(const std::string& body) {
    std::string path = "infcore_test_cfg.json";
    std::ofstream(path) << body;
    return path;
}
static bool load_throws(const std::string& body) {
    try { infcore::load_config(write_tmp(body)); return false; }
    catch (const std::exception&) { return true; }
}

static void test_config() {
    {
      std::ofstream("infcore_test_model.gguf", std::ios::binary) << "abc";
    }
    const char* valid = R"({
      "server": {"host":"127.0.0.1","port":8080},
      "security": {"rbac_enabled":true,
        "principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
      "runtime": {"llama_server_bin":"/bin/true"},
      "models": [{"logical_name":"m","gguf_path":"/tmp/m.gguf"}]
    })";
    bool loaded = true;
    try { infcore::load_config(write_tmp(valid)); } catch (...) { loaded = false; }
    CHECK(loaded);

    // заглушечный ключ
    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":false,"principals":[{"api_key":"change-me-x","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
      "models":[{"logical_name":"m","backend_url":"http://127.0.0.1:9"}]})"));

    // vision без mmproj
    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
      "runtime":{"llama_server_bin":"/bin/true"},
      "models":[{"logical_name":"v","gguf_path":"/tmp/v.gguf","modality":"vision"}]})"));

    // enforce_no_egress + внешний нелокальный backend_url
    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
      "offline":{"enforce_no_egress":true},
      "models":[{"logical_name":"e","backend_url":"http://8.8.8.8:1234"}]})"));

    // роль principal'а не объявлена
    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":true,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"ghost"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
      "models":[{"logical_name":"m","backend_url":"http://127.0.0.1:9"}]})"));

    // M1: обход egress-проверки через userinfo (реальный хост evil.com) -> отказ
    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
      "offline":{"enforce_no_egress":true},
      "models":[{"logical_name":"e","backend_url":"http://127.0.0.1@evil.com/"}]})"));

    // M1: обход по префиксу строки (хостнейм начинается с "127."/"10.") -> отказ
    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
      "offline":{"enforce_no_egress":true},
      "models":[{"logical_name":"e","backend_url":"http://127.0.0.1.evil.com:9/"}]})"));

    // M1: легитимный локальный RFC1918-бэкенд по-прежнему грузится
    {
      bool ok = true;
      try { infcore::load_config(write_tmp(R"({"server":{"host":"127.0.0.1","port":8080},
        "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
          "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
        "offline":{"enforce_no_egress":true},
        "models":[{"logical_name":"e","backend_url":"http://192.168.10.5:8100"}]})")); }
      catch (...) { ok = false; }
      CHECK(ok);
    }

    // rerank-модель валидируется как отдельная модальность
    {
      bool ok = true;
      try { infcore::load_config(write_tmp(R"({"server":{"host":"127.0.0.1","port":8080},
        "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
          "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
        "runtime":{"llama_server_bin":"/bin/true"},
        "models":[{"logical_name":"r","gguf_path":"/tmp/r.gguf","modality":"rerank"}]})")); }
      catch (...) { ok = false; }
      CHECK(ok);
    }

    // audit.require=true не может сочетаться с sink=none
    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}],
        "audit":{"sink":"none","require":true}},
      "models":[{"logical_name":"m","backend_url":"http://127.0.0.1:9"}]})"));

    // duplicate role / model references fail fast
    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["missing"],"allow_endpoints":["*"]}]},
      "models":[{"logical_name":"m","backend_url":"http://127.0.0.1:9"}]})"));

    // release integrity gate: sha256 + size_bytes для managed model
    {
      bool ok = true;
      try { infcore::load_config(write_tmp(R"({"server":{"host":"127.0.0.1","port":8080},
        "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
          "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
        "offline":{"require_model_integrity":true},
        "runtime":{"llama_server_bin":"/bin/true"},
        "models":[{"logical_name":"m","gguf_path":"infcore_test_model.gguf",
          "sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "size_bytes":3}]})")); }
      catch (...) { ok = false; }
      CHECK(ok);
    }

    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
      "offline":{"require_model_integrity":true},
      "runtime":{"llama_server_bin":"/bin/true"},
      "models":[{"logical_name":"m","gguf_path":"infcore_test_model.gguf"}]})"));

    CHECK(load_throws(R"({"server":{"host":"127.0.0.1","port":8080},
      "security":{"rbac_enabled":false,"principals":[{"api_key":"0123456789abcdef01234567","subject":"a","role":"admin"}],
        "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}]},
      "runtime":{"llama_server_bin":"/bin/true"},
      "models":[{"logical_name":"m","gguf_path":"infcore_test_model.gguf",
        "sha256":"0000000000000000000000000000000000000000000000000000000000000000",
        "size_bytes":3}]})"));

    std::remove("infcore_test_cfg.json");
    std::remove("infcore_test_model.gguf");
}

static void test_supervisor_token_failure() {
    infcore::BackendSupervisor::Options opt;
    opt.llama_server_bin = "/bin/true";
    opt.backend_token_path = "/definitely/missing/infcore-token-source";
    bool threw = false;
    try {
        infcore::BackendSupervisor sup(opt);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    test_rbac();
    test_authn();
    test_json_schema();
    test_config();
    test_supervisor_token_failure();
    std::printf("infcore unit tests: %d/%d passed\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
