#define FUSE_USE_VERSION 30
#include "AsyncRead.hpp"

#include <fuse3/fuse_lowlevel.h>

#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace fs = std::filesystem;

using tcp = net::ip::tcp;
using file_size_t = size_t; // size_t в read передаётся
using lookup_t = std::pair<bool, file_size_t>;
using ws_type = websocket::stream<typename beast::tcp_stream::rebind_executor<
    typename net::use_awaitable_t<>::executor_with_default<
        net::any_io_executor>>::other>;

std::string fuse_dir = "mountpoint";
const char* host = "localhost";
const char* port = "8765";

const int kMaxMessage = 100000; //  100000  ws.async_write(net::buffer(std::string(100000, 'A')),
                                //  on_write); 100000 ok, but +1 bad:( hahah \0x00
const int kFnameSize = 10;
const int kChunkHeaderSize = 5;
const int kSmallFileHeader = 1 + kFnameSize;
const int kBigFileHeader = kSmallFileHeader + kChunkHeaderSize;
const int kBigFileUsefullyPart = kMaxMessage - kBigFileHeader;

static_assert(kMaxMessage >= kSmallFileHeader);

bool is_regular_file(const std::string& path)
{
    return fs::exists(path) && fs::is_regular_file(path) && path.find('/') == std::string::npos;
}

inline auto to_buffer(const std::string& s)
{
    return boost::asio::buffer(s.data(), s.size()); // without  \0x00
}

// https://stackoverflow.com/a/8317622
#define A 54059 /* a prime */
#define B 76963 /* another prime */
#define FIRSTH 37 /* also prime */

unsigned hash_str(const char* s)
{
    unsigned h = FIRSTH;
    while (*s) {
        h = (h * A) ^ (s[0] * B);
        s++;
    }
    return h;
}

// TODO добавить ко всем методам в сокет  hash_str

template <typename T>
std::string format_int(T x, int len)
{
    std::string s = std::to_string(x);
    if (s.size() < len) {
        int num_zeros = len - s.length();
        s = std::string(num_zeros, '0') + s;
    }
    return s;
}

// read|unsigned_fname_hash|fname
inline auto read_command(const std::string& fname)
{
    return "read" + format_int(hash_str(fname.c_str()), kFnameSize) + fname;
}

inline auto lookup_command(const std::string& fname)
{
    return "lookup" + fname;
}

template <typename T>
std::string buffers_to_string_consume(T& buffer)
{
    std::string res = beast::buffers_to_string(buffer.data());
    buffer.consume(buffer.size());
    return res;
}

std::vector<std::string> split(const std::string& s,
    const std::string& delimiter)
{
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;
    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr(pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        if (!token.empty()) // !!!
            res.push_back(token);
    }
    res.push_back(s.substr(pos_start));
    return res;
}

// ================

class Client {
public:
    ws_type ws;

protected:
    beast::flat_buffer buffer;

public:
    Client(net::io_context& ioc)
        : ws(net::make_strand(ioc))
    {
    }

    net::awaitable<void> handshake(const char* host, const char* port)
    {
        // https://github.com/boostorg/beast/blob/0afa15e8cb4cfe5257a2dc44dab9cca657520258/example/websocket/client/awaitable/websocket_client_awaitable.cpp#L63C5-L63C45
        auto resolver = boost::asio::use_awaitable_t<boost::asio::any_io_executor>::
            as_default_on(tcp::resolver(co_await net::this_coro::executor));
        auto const results = co_await resolver.async_resolve(host, port);
        auto ep = co_await beast::get_lowest_layer(ws).async_connect(results);
        co_await ws.async_handshake(
            std::string(host) + ':' + std::to_string(ep.port()), "/");
        co_return;
    }
};

class HostSystem : public Client,
                   public std::enable_shared_from_this<HostSystem> {
public:
    HostSystem(net::io_context& ioc)
        : Client(ioc)
    {
    }

    net::awaitable<void> send_large_message(const std::string& fhash,
        std::vector<char>& data)
    {
        // тут явно можно улучшать, только как я понял ws.async_write принимает
        // только beast::net::buffer который в свою очередь тупо кусок памяти
        // подряд, как и вектор. Поэтому лучшее решение тупо брать и memcopy в
        // однократно созданный буффер. implemented

        // 1fhash|total_chunks|chunk_buf а потом всегда 2fhash|cur_chunk|chunk_buf

        const int total_chunk = (data.size() + kBigFileUsefullyPart - 1) / kBigFileUsefullyPart;
        assert(total_chunk <= CHUNK_TOTAL_MAX);

        std::vector<char> buf;
        buf.resize(kMaxMessage);

        for (int i = 0; i < kFnameSize; i++)
            buf[1 + i] = fhash[i]; // todo memcpy

        auto chunk_header = [&](int i) {
            // TODO improve buf[10 + _] = chunk[_]
            std::string chunk = format_int(i == 0 ? total_chunk : i, kChunkHeaderSize);
            for (int _ = 0; _ < kChunkHeaderSize; _++)
                buf[kSmallFileHeader + _] = chunk[_];
            if (i != 0) {
                buf[0] = '2';
            } else {
                buf[0] = '1';
            }
        };

        for (int i = 0; i < total_chunk - 1; i++) {
            chunk_header(i);
            std::memcpy(buf.data() + kBigFileHeader,
                static_cast<char*>(data.data()) + static_cast<file_size_t>(i) * kBigFileUsefullyPart,
                sizeof(char) * kBigFileUsefullyPart);
            co_await ws.async_write(net::buffer(buf));
        }
        {
            chunk_header(total_chunk - 1);
            const size_t remain = data.size() % kBigFileUsefullyPart;
            std::memcpy(
                buf.data() + kBigFileHeader,
                static_cast<char*>(data.data()) + static_cast<file_size_t>(total_chunk - 1) * kBigFileUsefullyPart,
                sizeof(char) * remain);
            co_await ws.async_write(net::buffer(buf.data(), kBigFileHeader + remain));
        }
        co_return;
    }

    net::awaitable<void> read_commands()
    {
        while (true) {
            co_await ws.async_read(buffer);
            const auto data = buffers_to_string_consume(buffer);

            if (data == "exit") {
                co_return;
            }

            if (data.rfind("read", 0) == 0) {
                co_await ws.async_read(buffer);
                off_t off = std::stoll(buffers_to_string_consume(buffer));

                co_await ws.async_read(buffer);
                size_t size = std::stoll(buffers_to_string_consume(buffer));
                std::string fhash = data.substr(4, kFnameSize);
                auto fname = std::string_view(data).substr(4 + kFnameSize);

                auto future = async_io::read(fname.data(), off, size);
                std::vector<char> buf = future.get();

                // logical
                // small file - 0fhash|buf
                // bif file - 1/2|fhash|total/cur_chunks|chunk_buf

                ws.binary(true);
                if (kSmallFileHeader + buf.size() <= kMaxMessage) {
                    buf.insert(buf.begin(), fhash.begin(), fhash.end());
                    buf.insert(buf.begin(), '0');
                    co_await ws.async_write(net::buffer(buf));
                } else {
                    co_await send_large_message(fhash, buf);
                }
                ws.binary(false);
            } else if (data.rfind("lookup", 0) == 0) {
                auto fname = data.substr(6);
                if (!is_regular_file(fname)) {
                    co_await ws.async_write(to_buffer("-"));
                } else {
                    std::ifstream in(fname, std::ifstream::ate | std::ifstream::binary);
                    if (in.fail()) {
                        co_await ws.async_write(to_buffer("-"));
                    } else {
                        co_await ws.async_write(to_buffer(
                            std::to_string(static_cast<file_size_t>(in.tellg()))));
                    }
                }
            } else if (data.rfind("ls", 0) == 0) {
                std::string res;
                std::string path = ".";
                for (const auto& entry : fs::directory_iterator(path)) {
                    std::error_code ec;
                    if (entry.path().filename().generic_string() == fuse_dir) {
                        // skip fuse dirs https://github.com/boostorg/filesystem/issues/113
                        continue;
                    }
                    if (fs::is_regular_file(entry.path(), ec)) {
                        res += entry.path();
                    }
                }
                co_await ws.async_write(to_buffer(res));
            }
        }
        co_return;
    }

    net::awaitable<std::string> connect(const char* host, const char* port)
    {
        co_await Client::handshake(host, port);
        co_await ws.async_write(to_buffer("host"));
        co_await ws.async_read(buffer);
        co_return buffers_to_string_consume(buffer); // uuid
    }
};

class ClientSystem : public Client,
                     public std::enable_shared_from_this<ClientSystem> {
private:
    net::io_context& ioc;

public:
    ClientSystem(net::io_context& ioc)
        : ioc(ioc)
        , Client(ioc)
    {
    }

    net::awaitable<bool> connect(const char* host, const char* port,
        const char* uuid)
    {
        co_await Client::handshake(host, port);
        co_await ws.async_write(to_buffer("client" + std::string(uuid)));
        co_await ws.async_read(buffer);
        co_return buffers_to_string_consume(buffer) == "ok";
    }

    net::awaitable<file_size_t> async_read_file(std::string& fname, char*& buf,
        off_t off, size_t size)
    {
        {
            const std::string msg = read_command(fname);
            co_await ws.async_write(to_buffer(msg));
            co_await ws.async_write(to_buffer(std::to_string(off)));
            co_await ws.async_write(to_buffer(std::to_string(size)));
        }
        co_await ws.async_read(buffer);
        char* c = boost::asio::buffer_cast<char*>(buffer.data());

        if (c[0] == '0') {
            size_t buffer_size = buffer.size();
            buffer.consume(size + 11);
            buf = const_cast<char*>(c + kSmallFileHeader);
            co_return buffer_size - kSmallFileHeader;
        } else {
            size_t total_chunks = std::stoll(std::string(c + kSmallFileHeader, kChunkHeaderSize));

            buf = new char[static_cast<file_size_t>(total_chunks) * kBigFileUsefullyPart];
            std::memcpy(
                buf, boost::asio::buffer_cast<char*>(buffer.data()) + kBigFileHeader,
                sizeof(char) * kBigFileUsefullyPart);
            buffer.consume(buffer.size() + 11);
            size_t cnt = 0;
            for (int i = 1; i < total_chunks; i++) {
                co_await ws.async_read(buffer);

                size_t cur_chunk = std::stoll(std::string(
                    boost::asio::buffer_cast<char*>(buffer.data()) + kSmallFileHeader,
                    kChunkHeaderSize));
                if (cur_chunk != total_chunks - 1) {
                    cnt = kBigFileUsefullyPart;
                } else {
                    cnt = (buffer.size() - kBigFileHeader);
                }

                std::memcpy(
                    buf + static_cast<file_size_t>(cur_chunk) * kBigFileUsefullyPart,
                    boost::asio::buffer_cast<char*>(buffer.data()) + kBigFileHeader,
                    sizeof(char) * cnt);

                buffer.consume(buffer.size() + 11);
            }
            co_return static_cast<file_size_t>(total_chunks - 1) * kBigFileUsefullyPart + cnt;
        }
    }

    net::awaitable<lookup_t> async_lookup_file(const char* fname)
    {
        {
            const std::string msg = lookup_command(std::string(fname));
            co_await ws.async_write(to_buffer(msg));
        }
        co_await ws.async_read(buffer);
        auto data = buffers_to_string_consume(buffer);
        if (data == "-") {
            co_return lookup_t { false, 0 };
        }
        file_size_t size = std::stoll(data);
        co_return lookup_t { true, size };
    }

    net::awaitable<std::vector<std::string>> async_ls()
    {
        co_await ws.async_write(to_buffer("ls"));
        co_await ws.async_read(buffer);
        auto data = buffers_to_string_consume(buffer);
        co_return split(data, "./");
    }

    lookup_t lookup_file(const char* name)
    {
        return co_spawn(ioc, async_lookup_file(name), boost::asio::use_future)
            .get();
    }

    file_size_t read_file(std::string& fname, char*& buf, off_t off,
        size_t size)
    {
        return co_spawn(ioc, async_read_file(fname, buf, off, size),
            boost::asio::use_future)
            .get();
    }

    std::vector<std::string> ls()
    {
        return co_spawn(ioc, async_ls(), boost::asio::use_future).get();
    }
};

void help()
{
    std::cerr << "usage ./client host_uuid\n"
                 "like ./client ff69c95c2aa94b65b6914ecfaba89302"
              << std::endl;
}

// fuse part

class ClientIO {
    // можно потом описать класс в котором много вебсокетов для более сложной
    // логики
private:
    std::shared_ptr<ClientSystem> client;

    std::mutex mutex;

public:
    void set_client(std::shared_ptr<ClientSystem> client_) { client = client_; }

    lookup_t lookup_file(const char* name)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return client->lookup_file(name);
    }

    file_size_t read_file(std::string& fname, char*& buf, off_t off,
        size_t size)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return client->read_file(fname, buf, off, size);
    }

    std::vector<std::string> ls()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return client->ls();
    }
} ClientIO;

class lookup_manager {
private:
    // f*ck char*
    std::map<std::string, fuse_ino_t> lookup_map {};
    std::vector<std::string> names = { ".", "." };
    fuse_ino_t last_ino = 1; // 1=FUSE_ROOT_ID
    std::mutex mutex;

public:
    fuse_ino_t get_ino(const char* name_)
    {
        std::unique_lock<std::mutex> lock(mutex);
        std::string name(name_);
        auto it = lookup_map.find(name);
        if (it != lookup_map.end()) {
            return it->second;
        } else {
            last_ino++;
            names.push_back(name);
            lookup_map[name] = last_ino;
            return last_ino;
        }
    }

    std::string get_name(fuse_ino_t ino)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return names[ino];
    }

    int insert(const std::string& name)
    {
        std::unique_lock<std::mutex> lock(mutex);
        auto it = lookup_map.find(name);
        if (it != lookup_map.end()) {
            return it->second;
        }
        last_ino++;
        names.push_back(name);
        lookup_map[name] = last_ino;
        return last_ino;
    }
} lookup_manager {};

void attr_init(struct stat& attr, lookup_t lookup, fuse_ino_t ino)
{
    attr.st_ino = ino;
    attr.st_mode = S_IFREG | 0444; // Обычный файл с правами чтения
    attr.st_nlink = 1; // Число ссылок
    attr.st_size = lookup.second; // Размер файла (очень важно для вывода файла)
}

void lookup(fuse_req_t req, fuse_ino_t parent, const char* name)
{
    // lookup не всегда вызывается, у fuse есть кэш, причём заметный даже обычному
    // пользователю.

    // работаем без папок и файл должен существовать
    if (parent == FUSE_ROOT_ID) {
        lookup_t lookup = ClientIO.lookup_file(name);
        if (lookup.first) {
            struct fuse_entry_param e { };

            memset(&e, 0, sizeof(e));

            e.ino = lookup_manager.get_ino(name);
            e.attr_timeout = 3; // кэширование атрибутов на 3 секунды
            e.entry_timeout = 3; // кэширование записи на 3 секунды
            attr_init(e.attr, lookup, e.ino);
            fuse_reply_entry(req, &e);

            return;
        }
    }
    fuse_reply_err(req, ENOENT);
}

static int reply_buf_limited(fuse_req_t req, const char* buf, size_t bufsize,
    off_t off, size_t maxsize)
{
    if (off < bufsize)
        return fuse_reply_buf(req, buf + off, std::min(bufsize - off, maxsize));
    else
        return fuse_reply_buf(req, NULL, 0);
}

void getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi)
{
    // по уму надо делать fstatat, но я устану перекидывать всё через сокеты...
    struct stat attr;
    memset(&attr, 0, sizeof(attr));
    std::string fname = lookup_manager.get_name(ino);
    lookup_t lookup = ClientIO.lookup_file(fname.c_str());
    if (ino == 1) {
        // stat(".", &attr);
        attr.st_ino = FUSE_ROOT_ID; // ID корневой директории
        attr.st_mode = S_IFDIR | 0755; // Тип: директория, права: 755
        attr.st_nlink = 1; // Количество hard links
        fuse_reply_attr(req, &attr, 1.0);
        return;
    }
    attr_init(attr, lookup, ino);
    fuse_reply_attr(req, &attr, 1.0);
}

struct dirbuf {
    char* p;
    size_t size;
};

static void dirbuf_add(fuse_req_t req, struct dirbuf* b, const char* name,
    fuse_ino_t ino)
{
    // legacy part from
    // https://github.com/libfuse/libfuse/blob/master/example/hello_ll.c#L92
    struct stat stbuf;
    size_t oldsize = b->size;
    b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
    b->p = (char*)realloc(b->p, b->size);
    memset(&stbuf, 0, sizeof(stbuf));
    stbuf.st_ino = ino;

    if (ino != 1) {
        std::string fname = lookup_manager.get_name(ino);
        lookup_t lookup = ClientIO.lookup_file(fname.c_str());
        attr_init(stbuf, lookup, ino);
    }

    fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf,
        b->size);
}

void readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info* fi)
{
    if (ino != 1)
        fuse_reply_err(req, ENOTDIR);
    else {
        struct dirbuf b;
        memset(&b, 0, sizeof(b));
        dirbuf_add(req, &b, ".", 1);
        dirbuf_add(req, &b, "..", 1);
        for (const auto& i : ClientIO.ls()) {
            dirbuf_add(req, &b, i.c_str(), lookup_manager.insert(i));
        }
        reply_buf_limited(req, b.p, b.size, off, size);
        free(b.p);
    }
}

void read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info* fi)
{
    std::string fname = lookup_manager.get_name(ino);
    lookup_t lookup = ClientIO.lookup_file(fname.c_str());
    char* fbuf = nullptr;
    file_size_t my_size = ClientIO.read_file(fname, fbuf, off, std::min(size, lookup.second - off));

    // reply_buf_limited(req, buf, std::min(my_size, size), off, size); --- фигня
    // https://github.com/nuagelabsfr/cloud-gateway/blob/5d59f2d94346e22269678e097887cd7d904c8e7f/src/cloudFUSE/cloudFUSE_low.c#L787
    // https://github.com/nuagelabsfr/cloud-gateway/blob/5d59f2d94346e22269678e097887cd7d904c8e7f/src/cloudFUSE/cloudFUSE_low.c#L453
    // размеры все совпадают всё изи
    struct fuse_bufvec buf = FUSE_BUFVEC_INIT(
        my_size); // not size => так как fbuf начнёт читаться за границами
    buf.buf[0].mem = (char*)fbuf;
    fuse_reply_data(req, &buf, FUSE_BUF_SPLICE_NONBLOCK);

    // delete fbuf; --- не надо fuse_reply_data сам очистит как это не странно
}

static void open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
{
    // проверка на O_RDONLY открытие бесполезная, так как я всем дал O_RDONLY в
    // lookup
    fuse_reply_open(req, fi);
}

static struct fuse_lowlevel_ops fuse_example_oper {
    .lookup = lookup, // name -> inode custom xD
        .getattr = getattr, // обновляем атрибуты файла
        .open = open, // проверяем права --- 1 строка
        .read = read, // ok можем ли читать
        .readdir = readdir, // директория выводится
};

struct raii_fuse_args {
    fuse_args args;
    char* myargv[2];

    raii_fuse_args(char* argv0)
    {
        myargv[0] = argv0;
        myargv[1] = fuse_dir.data();
        args = FUSE_ARGS_INIT(2, myargv);
    }

    ~raii_fuse_args() { fuse_opt_free_args(&args); }
};

struct raii_fuse_cmdline_opts {
    struct fuse_cmdline_opts opts { };

    raii_fuse_cmdline_opts() = default;

    char* get_mountpoint() { return opts.mountpoint; }

    ~raii_fuse_cmdline_opts()
    {
        free(opts.mountpoint); // char* mounpoint
    }
};

namespace {
std::function<void(int)> shutdown_handler;

void signal_handler(int signal) { shutdown_handler(signal); }
} // namespace

void client_system(char* argv[])
{
    std::signal(SIGINT, signal_handler);
    struct fuse_session* se;

    shutdown_handler = [&se](int signal) {
        fuse_session_unmount(se);
        fuse_session_destroy(se);
    };

    raii_fuse_args args { argv[0] };
    raii_fuse_cmdline_opts opts {};

    {
        if (fuse_parse_cmdline(&args.args, &opts.opts) != 0) {
            exit(1);
        }
        se = fuse_session_new(&args.args, &fuse_example_oper,
            sizeof(fuse_example_oper), NULL);
        if (se == NULL) {
            exit(1);
        }
        if (fuse_session_mount(se, opts.get_mountpoint()) != 0) {
            fuse_session_destroy(se);
            exit(1);
        }
    }

    net::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    auto client = std::make_shared<ClientSystem>(ioc);
    ClientIO.set_client(client);

    auto connection_f = boost::asio::co_spawn(
        ioc, client->connect(host, port, argv[1]), net::use_future);
    std::thread asio_thread([&]() { ioc.run(); });
    asio_thread.detach();
    bool connection_status = connection_f.get();
    if (connection_status) {
        std::cout << "connection successful" << std::endl;
    } else {
        std::cerr << "connection failed" << std::endl;
        raise(SIGINT);
    }
    std::thread fuse_loop { [&] { fuse_session_loop(se); } };
    fuse_loop.join();
}

void host_system()
{
    net::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    auto client = std::make_shared<HostSystem>(ioc);
    boost::asio::co_spawn(
        ioc,
        [&]() -> net::awaitable<void> {
            std::string uuid = co_await client->connect(host, port);
            std::cout << "uuid : " << uuid << std::endl;
            co_await client->read_commands();
        },
        net::detached);
    ioc.run();
}

int main(int argc, char* argv[])
{
    try {
        if (argc == 1) {
            host_system();
        } else if (argc == 2 && strcmp(argv[1], "-h") == 0) {
            help();
        } else if (argc == 2) {
            client_system(argv);
        } else {
            help();
            return 1;
        }
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        exit(-1);
    }
    return 0;
}