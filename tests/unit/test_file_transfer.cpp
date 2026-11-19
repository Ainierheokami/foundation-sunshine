/**
 * @file tests/unit/test_file_transfer.cpp
 * @brief Files offered through the clipboard: listing, ids, ranges and the HTTP answers.
 */
#include "src/config.h"
#include "src/file_transfer.h"
#include "src/file_transfer_http.h"

#include <array>
#include <boost/asio.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <regex>
#include <thread>

namespace fs = std::filesystem;

namespace {
  std::string
  utf8(const fs::path &p) {
    const auto s = p.u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
  }

  class FileTransfer: public testing::Test {
  protected:
    fs::path root;
    bool previous_sync = false;

    void
    SetUp() override {
      previous_sync = config::input.clipboard_sync;
      config::input.clipboard_sync = true;
      root = fs::temp_directory_path() / ("file_transfer_test_" + std::to_string(std::random_device {}()));
      fs::create_directories(root / "dir" / "sub");
      write(root / "a.txt", "hello");
      write(root / "dir" / "b.bin", std::string(1000, 'x'));
      write(root / "dir" / "sub" / "c.txt", "");
      file_transfer::clear();
    }

    void
    TearDown() override {
      file_transfer::clear();
      std::error_code ec;
      fs::remove_all(root, ec);
      config::input.clipboard_sync = previous_sync;
    }

    static void
    write(const fs::path &p, const std::string &content) {
      std::ofstream out(p, std::ios::binary);
      out << content;
    }

    /// Run handle_download against a recording channel; returns the JSON written, or "<file>" when a file was admitted.
    std::string
    download(const std::string &id, const std::string &index, const web::CaseInsensitiveMultimap &headers = {}) {
      auto req = std::make_shared<web::Request>();
      req->method = "GET";
      req->path = "/api/v1/file-transfer/" + id + "/" + index;
      req->header = headers;
      static const std::regex pattern("^/api/v1/file-transfer/([0-9a-f\\-]{36})/([0-9]{1,6})$");
      EXPECT_TRUE(std::regex_match(req->path, req->path_match, pattern));
      std::string output;
      bool file = false;
      web::detail::ResponseChannel channel;
      channel.send = [&output](std::string bytes, auto completion, bool, bool) {
        output += bytes;
        if (completion) completion(web::error_code {});
      };
      channel.begin_stream = [] {};
      channel.closed = [] { return false; };
      channel.abort = [] {};
      channel.send_file = [&file](auto) { file = true; };
      file_transfer_http::https_server_t::Response response(*req, std::move(channel));
      file_transfer_http::handle_download(response, *req);
      response.send();
      return file ? std::string("<file>") : output;
    }
  };
}  // namespace

TEST_F(FileTransfer, ListsFilesAndFoldersParentsFirst) {
  const auto result = file_transfer::register_paths({ utf8(root / "a.txt"), utf8(root / "dir") }, "clipboard");
  ASSERT_TRUE(result.offer) << result.error;
  const auto &entries = result.offer->entries;
  ASSERT_EQ(entries.size(), 5u);
  EXPECT_EQ(entries[0].path, "a.txt");
  EXPECT_EQ(entries[0].size, 5u);
  EXPECT_FALSE(entries[0].dir);
  EXPECT_EQ(entries[1].path, "dir");
  EXPECT_TRUE(entries[1].dir);
  std::map<std::string, std::size_t> position;
  for (std::size_t i = 0; i < entries.size(); ++i) position[entries[i].path] = i;
  ASSERT_TRUE(position.contains("dir/sub"));
  ASSERT_TRUE(position.contains("dir/sub/c.txt"));
  ASSERT_TRUE(position.contains("dir/b.bin"));
  EXPECT_LT(position["dir/sub"], position["dir/sub/c.txt"]);
  EXPECT_EQ(result.offer->total_size, 1005u);
  EXPECT_TRUE(file_transfer::is_valid_id(result.offer->id));
  EXPECT_EQ(result.offer->type, "clipboard");

  const auto json = file_transfer::offer_json(*result.offer);
  EXPECT_EQ(json["files"].size(), 5u);
  EXPECT_EQ(json["files"][0]["path"], "a.txt");
  EXPECT_GT(json["expires_in"].get<int>(), 0);
}

TEST_F(FileTransfer, RefusesWhatItCannotOffer) {
  EXPECT_EQ(file_transfer::register_paths({}, "clipboard").error, "no_paths");
  EXPECT_EQ(file_transfer::register_paths({ utf8(root / "missing") }, "clipboard").error, "not_found");
  EXPECT_EQ(file_transfer::register_paths({ "relative/a.txt" }, "clipboard").error, "not_found");
  fs::create_directories(root / "other");
  write(root / "other" / "a.txt", "x");
  EXPECT_EQ(file_transfer::register_paths({ utf8(root / "a.txt"), utf8(root / "other" / "a.txt") }, "clipboard").error, "duplicate_name");
}

TEST_F(FileTransfer, RelativePathsStayInsideTheOfferAndFitMaxPath) {
  EXPECT_EQ(file_transfer::relative_path("dir", fs::path("sub") / "c.txt"), "dir/sub/c.txt");
  EXPECT_EQ(file_transfer::relative_path("a.txt", {}), "a.txt");
  EXPECT_FALSE(file_transfer::relative_path("dir", fs::path("..") / "x"));
  EXPECT_FALSE(file_transfer::relative_path("", "x"));
  EXPECT_TRUE(file_transfer::relative_path("d", std::string(257, 'x')));
  EXPECT_FALSE(file_transfer::relative_path("d", std::string(258, 'x')));
  // Characters outside the BMP take two UTF-16 units each.
  std::string emoji;
  for (int i = 0; i < 128; ++i) emoji += "\xF0\x9F\x98\x80";
  EXPECT_TRUE(file_transfer::relative_path("d", fs::path(std::u8string(reinterpret_cast<const char8_t *>(emoji.data()), emoji.size()))));
  emoji += "\xF0\x9F\x98\x80";
  EXPECT_FALSE(file_transfer::relative_path("d", fs::path(std::u8string(reinterpret_cast<const char8_t *>(emoji.data()), emoji.size()))));
}

TEST_F(FileTransfer, OnlyOpenEndedRangesAreHonoured) {
  bool unsatisfiable = false;
  EXPECT_EQ(file_transfer::parse_range("bytes=10-", 100, unsatisfiable), 10u);
  EXPECT_FALSE(unsatisfiable);
  EXPECT_FALSE(file_transfer::parse_range("bytes=0-", 0, unsatisfiable));
  EXPECT_FALSE(unsatisfiable);
  EXPECT_FALSE(file_transfer::parse_range("bytes=100-", 100, unsatisfiable));
  EXPECT_TRUE(unsatisfiable);
  EXPECT_FALSE(file_transfer::parse_range("bytes=1-5", 100, unsatisfiable));
  EXPECT_FALSE(unsatisfiable);
  EXPECT_FALSE(file_transfer::parse_range("bytes=-5", 100, unsatisfiable));
  EXPECT_FALSE(file_transfer::parse_range("items=1-", 100, unsatisfiable));
  EXPECT_FALSE(file_transfer::parse_range("bytes=1-,5-", 100, unsatisfiable));
}

TEST_F(FileTransfer, OpensFilesOnlyWhileUnchanged) {
  const auto result = file_transfer::register_paths({ utf8(root / "a.txt"), utf8(root / "dir") }, "send");
  ASSERT_TRUE(result.offer);
  const auto &id = result.offer->id;
  EXPECT_EQ(result.offer->type, "send");

  {
    auto opened = file_transfer::open_entry(id, "0");
    ASSERT_TRUE(opened.file.valid()) << opened.error;
    EXPECT_EQ(opened.file.size(), 5u);
  }

  EXPECT_EQ(file_transfer::open_entry(id, "1").error, "not_found");  // a folder
  EXPECT_EQ(file_transfer::open_entry(id, "99").error, "not_found");
  EXPECT_EQ(file_transfer::open_entry(id, "x").error, "not_found");
  EXPECT_EQ(file_transfer::open_entry("00000000-0000-4000-8000-000000000000", "0").error, "not_found");

  write(root / "a.txt", "hello, world");
  EXPECT_EQ(file_transfer::open_entry(id, "0").error, "changed");
  fs::remove(root / "a.txt");
  EXPECT_EQ(file_transfer::open_entry(id, "0").error, "gone");
}

TEST_F(FileTransfer, OldestOfferIsDroppedPastTheLimit) {
  std::vector<std::string> ids;
  for (std::size_t i = 0; i <= file_transfer::kMaxOffers; ++i) {
    ids.push_back(file_transfer::register_paths({ utf8(root / "a.txt") }, "clipboard").offer->id);
  }
  EXPECT_EQ(file_transfer::open_entry(ids.front(), "0").error, "not_found");
  EXPECT_TRUE(file_transfer::open_entry(ids.back(), "0").file.valid());
}

TEST_F(FileTransfer, RegisterResponseFollowsTheClipboardSetting) {
  const auto body = nlohmann::json { { "paths", { utf8(root / "a.txt") } } }.dump();
  auto ok = file_transfer_http::make_register_response(body);
  EXPECT_EQ(ok.status, web::StatusCode::ok);
  EXPECT_EQ(nlohmann::json::parse(ok.body)["type"], "clipboard");

  // The first agent release sent a single "path".
  auto legacy = file_transfer_http::make_register_response(nlohmann::json { { "path", utf8(root / "a.txt") } }.dump());
  EXPECT_EQ(legacy.status, web::StatusCode::ok);
  EXPECT_EQ(nlohmann::json::parse(legacy.body)["type"], "send");

  EXPECT_EQ(file_transfer_http::make_register_response("{").status, web::StatusCode::bad_request);
  EXPECT_EQ(file_transfer_http::make_register_response(R"({"paths":[1]})").status, web::StatusCode::bad_request);
  EXPECT_EQ(file_transfer_http::make_register_response(nlohmann::json { { "paths", { utf8(root / "nope") } } }.dump()).status, web::StatusCode::not_found);

  config::input.clipboard_sync = false;
  EXPECT_EQ(file_transfer_http::make_register_response(body).status, web::StatusCode::forbidden);
}

TEST_F(FileTransfer, DownloadAnswers) {
  const auto offer = file_transfer::register_paths({ utf8(root / "dir") }, "clipboard").offer;
  ASSERT_TRUE(offer);
  std::size_t file_index = 0;
  for (std::size_t i = 0; i < offer->entries.size(); ++i) {
    if (offer->entries[i].path == "dir/b.bin") file_index = i;
  }
  const auto index = std::to_string(file_index);

  EXPECT_EQ(download(offer->id, index), "<file>");
  EXPECT_EQ(download(offer->id, index, { { "Range", "bytes=10-" } }), "<file>");
  const auto past = download(offer->id, index, { { "Range", "bytes=1000-" } });
  EXPECT_NE(past.find("416"), std::string::npos);
  EXPECT_NE(past.find("bytes */1000"), std::string::npos);
  EXPECT_NE(download(offer->id, "0").find("404"), std::string::npos);  // the folder itself
  EXPECT_NE(download("00000000-0000-4000-8000-000000000000", index).find("404"), std::string::npos);

  config::input.clipboard_sync = false;
  EXPECT_NE(download(offer->id, index).find("403"), std::string::npos);
}

namespace {
  /// One GET over plain HTTP to a real listener; returns the raw response.
  std::string
  raw_get(unsigned short port, const std::string &path, const std::string &extra_headers = {}) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    socket.connect({ boost::asio::ip::make_address("127.0.0.1"), port });
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + extra_headers + "\r\n";
    boost::asio::write(socket, boost::asio::buffer(request));
    std::string response;
    std::array<char, 65536> buffer {};
    boost::system::error_code error;
    do {
      const auto size = socket.read_some(boost::asio::buffer(buffer), error);
      response.append(buffer.data(), size);
    } while (!error);
    return response;
  }

  std::string
  body_of(const std::string &response) {
    const auto end = response.find("\r\n\r\n");
    return end == std::string::npos ? std::string {} : response.substr(end + 4);
  }
}  // namespace

TEST_F(FileTransfer, ServesTheBytesOverARealListener) {
  std::string content(300000, '\0');
  for (std::size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>((i * 7) & 0xFF);
  write(root / "big.bin", content);
  const auto offer = file_transfer::register_paths({ utf8(root / "big.bin") }, "clipboard").offer;
  ASSERT_TRUE(offer);

  web::Server<web::HTTP> server;
  server.resource["^/api/v1/file-transfer/([0-9a-f\\-]{36})/([0-9]{1,6})$"]["GET"] = [](auto resp, auto req) {
    file_transfer_http::handle_download(*resp, *req);
  };
  server.config.address = "127.0.0.1";
  server.config.port = 0;
  std::promise<unsigned short> ready;
  auto bound = ready.get_future();
  std::thread listener([&] { server.start([&](unsigned short port) { ready.set_value(port); }); });
  ASSERT_EQ(bound.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto port = bound.get();
  const auto path = "/api/v1/file-transfer/" + offer->id + "/0";

  const auto whole = raw_get(port, path);
  EXPECT_TRUE(whole.starts_with("HTTP/1.1 200")) << whole.substr(0, 200);
  EXPECT_NE(whole.find("Accept-Ranges: bytes"), std::string::npos);
  EXPECT_EQ(body_of(whole), content);

  const auto tail = raw_get(port, path, "Range: bytes=123456-\r\n");
  EXPECT_TRUE(tail.starts_with("HTTP/1.1 206")) << tail.substr(0, 200);
  EXPECT_NE(tail.find("Content-Range: bytes 123456-299999/300000"), std::string::npos);
  EXPECT_EQ(body_of(tail), content.substr(123456));

  const auto closed_range = raw_get(port, path, "Range: bytes=0-9\r\n");
  EXPECT_TRUE(closed_range.starts_with("HTTP/1.1 200"));
  EXPECT_EQ(body_of(closed_range), content);

  server.stop();
  listener.join();
}
