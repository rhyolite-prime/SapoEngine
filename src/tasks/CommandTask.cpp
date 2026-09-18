//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//

#include "CommandTask.hpp"
#include "parser/AstNodes.hpp"
#include "runtime/ExpressionEvaluator.hpp"
#include "third_party/exprtk.hpp"
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <cstdlib>
#include <cpr/cpr.h>

namespace sapo::tasks {

// ---------------------------------------------------------------------------
// Helper: Simple Base64 encode for Basic Auth
// ---------------------------------------------------------------------------
static std::string base64_encode(const std::string &in) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::future<TaskOutcome> CommandTask::execute(TaskExecutionContext ctx) {

  auto command_node = std::static_pointer_cast<parser::CommandNode>(ctx.node_config);

  // Capture properties copy for thread safety
  std::string node_id = command_node->id.value_or("unnamed_command");
  std::string target_command = command_node->command;
  auto http_request_cfg = command_node->http_request;
  auto output_variable = command_node->output;

  // Launch directly via std::async and RETURN the future directly to the
  // interpreter loop!
  return std::async(std::launch::async, [node_id, target_command, http_request_cfg, output_variable, ctx]() mutable -> TaskOutcome {

      try {
          std::cout << "[Command Worker: " << node_id << "] Processing system command: '" << target_command << "'...\n";

          const bool is_http = (target_command.rfind("http.", 0) == 0);

          if (!is_http) {
              // Execute the system command natively if not HTTP
              std::cout << "   -> Executing shell command: " << target_command << "\n";
              int result = std::system(target_command.c_str());
              if (output_variable.has_value()) {
                  if (std::holds_alternative<std::string>(output_variable.value())) {
                      ctx.runtime_memory.setVariable(std::get<std::string>(output_variable.value()), (result == 0) ? "SUCCESS" : "FAILED");
                  }
              }
              return (result == 0) ? TaskOutcome::Success : TaskOutcome::Failed;
          }

          if (!http_request_cfg.has_value()) {
              throw std::runtime_error("Command '" + target_command + "' is an HTTP command but 'http_request' block is missing.");
          }

          const auto& cfg = http_request_cfg.value();

          // Resolve the URL (may contain a $variable reference)
          std::string resolved_url = sapo::runtime::ExpressionEvaluator::resolveValue(cfg.url, ctx.runtime_memory).get<std::string>();

          // Resolve headers map
          nlohmann::json resolved_headers = nlohmann::json::object();
          if (cfg.headers.has_value()) {
              resolved_headers = sapo::runtime::ExpressionEvaluator::resolveMap(cfg.headers.value(), ctx.runtime_memory);
          }

          // Handle Authentication config if present
          if (cfg.auth.has_value()) {
              const auto& auth = cfg.auth.value();
              if (auth.type == "basic") {
                  std::string r_user = sapo::runtime::ExpressionEvaluator::resolveValue(auth.username, ctx.runtime_memory).get<std::string>();
                  std::string r_pass = sapo::runtime::ExpressionEvaluator::resolveValue(auth.password, ctx.runtime_memory).get<std::string>();
                  std::string credentials = r_user + ":" + r_pass;
                  resolved_headers["Authorization"] = "Basic " + base64_encode(credentials);
              }
          }

          // Resolve query params
          nlohmann::json resolved_query = nlohmann::json::object();
          if (cfg.query.has_value()) {
              resolved_query = sapo::runtime::ExpressionEvaluator::resolveMap(cfg.query.value(), ctx.runtime_memory);
          }

          // Resolve body (either a string-map or a raw string expression)
          nlohmann::json resolved_body = nlohmann::json::object();
          if (cfg.body.has_value()) {
              std::visit([&](const auto& b) {
                  using T = std::decay_t<decltype(b)>;
                  if constexpr (std::is_same_v<T, std::string>) {
                      resolved_body = sapo::runtime::ExpressionEvaluator::resolveValue(b, ctx.runtime_memory);
                  } else {
                      resolved_body = sapo::runtime::ExpressionEvaluator::resolveMap(b, ctx.runtime_memory);
                  }
              }, cfg.body.value());
          }

          // Derive HTTP method from command suffix (e.g. "http.post" -> "POST")
          std::string method = target_command.substr(5); // strip "http."
          std::transform(method.begin(), method.end(), method.begin(), ::toupper);

          std::cout << "   -> [HTTP " << method << "] " << resolved_url << "\n";
          if (!resolved_headers.empty())
              std::cout << "   -> Headers: " << resolved_headers.dump() << "\n";
          if (!resolved_query.empty())
              std::cout << "   -> Query:   " << resolved_query.dump() << "\n";
          if (!resolved_body.empty())
              std::cout << "   -> Body:    " << resolved_body.dump() << "\n";
          if (cfg.timeout.has_value())
              std::cout << "   -> Timeout: " << cfg.timeout.value() << "ms\n";

          cpr::Session session;
          session.SetUrl(cpr::Url{resolved_url});

          // Set Headers
          if (!resolved_headers.empty()) {
              cpr::Header headers;
              for (auto& [k, v] : resolved_headers.items()) {
                  if (v.is_string()) {
                      headers[k] = v.get<std::string>();
                  } else {
                      headers[k] = v.dump();
                  }
              }
              session.SetHeader(headers);
          }

          // Set Query Parameters
          if (!resolved_query.empty()) {
              cpr::Parameters parameters;
              for (auto& [k, v] : resolved_query.items()) {
                  if (v.is_string()) {
                      parameters.Add({k, v.get<std::string>()});
                  } else {
                      parameters.Add({k, v.dump()});
                  }
              }
              session.SetParameters(parameters);
          }

          // Set Body
          if (!resolved_body.empty()) {
              if (resolved_body.is_string()) {
                  session.SetBody(cpr::Body{resolved_body.get<std::string>()});
              } else {
                  session.SetBody(cpr::Body{resolved_body.dump()});
              }
          }

          // Set Timeout
          if (cfg.timeout.has_value()) {
              session.SetTimeout(std::chrono::milliseconds(cfg.timeout.value()));
          }
          
          // Disable SSL verify for local dev/testing if needed (or assume it works)
          // session.SetVerifySsl(cpr::VerifySsl{false});

          cpr::Response r;
          if (method == "GET") {
              r = session.Get();
          } else if (method == "POST") {
              r = session.Post();
          } else if (method == "PUT") {
              r = session.Put();
          } else if (method == "DELETE") {
              r = session.Delete();
          } else if (method == "PATCH") {
              r = session.Patch();
          } else {
              throw std::runtime_error("Unsupported HTTP method: " + method);
          }
          
          if (r.error) {
              std::cerr << "   -> [HTTP Error] " << r.error.message << "\n";
              if (!r.text.empty()) {
                  std::cerr << "   -> Response Body: " << r.text << "\n";
              }
          } else {
              std::cout << "   -> Response Status: " << r.status_code << " | Time: " << (r.elapsed * 1000.0) << "ms\n";
              std::cout << "   -> Response Body: " << r.text << "\n";
          }

          // Capture Output Metadata
          if (output_variable.has_value()) {
              nlohmann::json response_metadata;
              response_metadata["status_code"] = r.status_code;
              response_metadata["elapsed_ms"] = r.elapsed * 1000.0;
              response_metadata["text"] = r.text;
              
              nlohmann::json parsed_json;
              bool is_json = false;
              if (!r.text.empty()) {
                  try {
                      parsed_json = nlohmann::json::parse(r.text);
                      response_metadata["json"] = parsed_json;
                      is_json = true;
                  } catch (...) {
                      // Not valid JSON, keep as text
                  }
              }
              
              std::visit([&](auto&& out_val) {
                  using T = std::decay_t<decltype(out_val)>;
                  if constexpr (std::is_same_v<T, std::string>) {
                      ctx.runtime_memory.setVariable(out_val, response_metadata);
                  } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                      if (is_json) {
                          for (const auto& key_path : out_val) {
                              nlohmann::json current = parsed_json;
                              bool found = true;
                              size_t start = 0;
                              size_t end = key_path.find('.');
                              while (end != std::string::npos) {
                                  std::string part = key_path.substr(start, end - start);
                                  if (current.is_object() && current.contains(part)) {
                                      current = current[part];
                                  } else {
                                      found = false; break;
                                  }
                                  start = end + 1;
                                  end = key_path.find('.', start);
                              }
                              if (found) {
                                  std::string part = key_path.substr(start);
                                  if (current.is_object() && current.contains(part)) {
                                      ctx.runtime_memory.setVariable(key_path, current[part]);
                                  } else if (current.is_array() && !part.empty() && std::isdigit(part[0])) {
                                      try {
                                          int index = std::stoi(part);
                                          if (index < current.size()) {
                                              ctx.runtime_memory.setVariable(key_path, current[index]);
                                          }
                                      } catch(...) {}
                                  }
                              }
                          }
                      }
                  }
              }, output_variable.value());
          }

          return (r.status_code >= 200 && r.status_code < 300) ? TaskOutcome::Success : TaskOutcome::Failed;

        } catch (const std::exception &e) {
          std::cerr << "[Command Worker Exception]: " << e.what() << "\n";
          return TaskOutcome::Failed;
        }
      });
}

} // namespace sapo::tasks
