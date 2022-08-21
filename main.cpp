#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/strand.hpp>
#include <boost/config.hpp>
#include <sstream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <yaml-cpp/yaml.h>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <regex>
#include "fs_track.hpp"


namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
namespace pt = boost::property_tree;

size_t file_maxsize;
std::set<std::string> dir_cache;

std::string dataCompress(const std::string& data)
{
    std::stringstream compressed;
    std::stringstream original;
    original << data;
    {
        boost::iostreams::filtering_streambuf<boost::iostreams::input> out;
        out.push(boost::iostreams::zlib_compressor());
        out.push(original);
        boost::iostreams::copy(out, compressed);

    }
    return compressed.str();
}

void dataDecompress(std::ifstream& data, std::ofstream& dst)
{
    boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
    in.push(boost::iostreams::zlib_decompressor());
    in.push(data);
    boost::iostreams::copy(in, dst);
}

std::string parsePayload(std::string& req_body) //There's no option in boost::http:parser a similar to .get()[payload]
{
    std::string payload = req_body;
    const std::regex borders{ "^--*"};
    std::smatch sm;
    std::regex_search(payload, sm, borders);

    std::string border = sm.str();

    size_t payload_beg = payload.find(border);
    size_t payload_end = payload.find(border, payload_beg + border.size() + 2);

    for(int i =0; i < 4; i++) //swifting through four \n lol
        payload_beg = payload.find('\n', payload_beg + 2);
    payload_beg++;
    payload = payload.substr(payload_beg, payload_end - 1 - payload_beg);

    return payload;
}


template<
    class Body, class Allocator,
    class Send>
    void
    handle_request(
        boost::beast::string_view doc_root,
        http::request<Body, http::basic_fields<Allocator>>&& req,
        Send&& send)
{
    if (req.method() == http::verb::post)
    {
        //gettin a filename from the field
        std::string path;
        {
            std::string body_string = req.body();
            const std::regex name_field{ "name=\".*\";" };
            std::smatch sm;
            std::regex_search(body_string, sm, name_field);
            path = sm.str();
            path = path.substr(6, path.size() - 8); // size - 1 - 6 - 1
        }
        path = doc_root.to_string() + path;

        // Oversize respond
        if (req.payload_size().value() > file_maxsize)
        {
            http::response<http::string_body> res{ http::status::payload_too_large, req.version() };
            return send(std::move(res));
        }

        //file compression and respond
        {
            if (std::filesystem::exists(path))
            {
                http::response<http::string_body> res{ http::status::conflict, req.version() };
                return send(std::move(res));
            }
                else
            {
                std::ofstream upload_file(path);

                std::string payload_str = parsePayload(req.body());
                std::string compressed_str = dataCompress(payload_str);

                std::cout << "Writing to: " << path << std::endl;
                upload_file << compressed_str;

				http::response<http::string_body> res{ http::status::ok, req.version() };
				return send(std::move(res));
            }
        }
    }

    if (req.method() == http::verb::get)
    {
        pt::ptree root; //json file parser
        std::stringstream ss;
        ss << req.body();

        pt::read_json(ss, root);

        std::string required_file = root.get<std::string>("filename:");

        if ( dir_cache.find(required_file) == end(dir_cache))
        {
            std::cout << "File" << required_file << "is not Found\n";
            http::response<http::string_body> res{ http::status::not_found, req.version() };
            return send(std::move(res));

        }
        else
        {
            beast::error_code ec;
            http::file_body::value_type body;

            std::ifstream ifsfile(doc_root.to_string() + required_file);
            std::ofstream ofstmp(doc_root.to_string() + "tmpfile");
            dataDecompress(ifsfile, ofstmp);
            body.open((doc_root.to_string() + "tmpfile").c_str(), beast::file_mode::scan, ec);

            http::response<http::file_body> res{
                std::piecewise_construct,
                std::make_tuple(std::move(body)),
                std::make_tuple(http::status::ok, req.version()) };
            return send(std::move(res));
        }
    }
}
//------------------------------------------------------------------------------
///Those classes below i took from boost docs and almost i have not changed anything

// Report a failure
void
fail(beast::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

// Handles an HTTP server connection
class session : public std::enable_shared_from_this<session>
{
    // The function object is used to send an HTTP message.
    struct send_lambda
    {
        session& self_;

        explicit
            send_lambda(session& self)
            : self_(self)
        {
        }

        template<bool isRequest, class Body, class Fields>
        void
            operator()(http::message<isRequest, Body, Fields>&& msg) const
        {
            // The lifetime of the message has to extend
            // for the duration of the async operation so
            // we use a shared_ptr to manage it.
            auto sp = std::make_shared<
                http::message<isRequest, Body, Fields>>(std::move(msg));

            // Store a type-erased version of the shared
            // pointer in the class to keep it alive.
            self_.res_ = sp;

            // Write the response
            http::async_write(
                self_.stream_,
                *sp,
                beast::bind_front_handler(
                    &session::on_write,
                    self_.shared_from_this(),
                    sp->need_eof()));
        }
    };

    size_t file_size;
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::shared_ptr<std::string const> doc_root_;
    http::request<http::string_body> req_;
    std::shared_ptr<void> res_;
    send_lambda lambda_;

public:
    // Take ownership of the stream
    session(
        tcp::socket&& socket,
        std::shared_ptr<std::string const> const& doc_root)
        : stream_(std::move(socket))
        , doc_root_(doc_root)
        , lambda_(*this)
    {
    }

    // Start the asynchronous operation
    void
        run()
    {
        do_read();
    }

    void
        do_read()
    {
        // Make the request empty before reading,
        // otherwise the operation behavior is undefined.
        req_ = {};

        // Set the timeout.
        stream_.expires_after(std::chrono::seconds(30));

        // Read a request
        http::async_read(stream_, buffer_, req_,
            beast::bind_front_handler(
                &session::on_read,
                shared_from_this()));
    }

    void
        on_read(
            beast::error_code ec,
            std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        // This means they closed the connection
        if (ec == http::error::end_of_stream)
            return do_close();

        if (ec)
            return fail(ec, "read");

        // Send the response
        handle_request(*doc_root_, std::move(req_), lambda_);
    }

    void
        on_write(
            bool close,
            beast::error_code ec,
            std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec)
            return fail(ec, "write");

        if (close)
        {
            // This means we should close the connection, usually because
            // the response indicated the "Connection: close" semantic.
            return do_close();
        }

        // We're done with the response so delete it
        res_ = nullptr;

        // Read another request
        do_read();
    }

    void
        do_close()
    {
        // Send a TCP shutdown
        beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);

        // At this point the connection is closed gracefully
    }
};

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener>
{
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::shared_ptr<std::string const> doc_root_; // const needless

public:
    listener(
        net::io_context& ioc,
        tcp::endpoint endpoint,
        std::shared_ptr<std::string const> const& doc_root)
        : ioc_(ioc)
        , acceptor_(net::make_strand(ioc))
        , doc_root_(doc_root)
    {
        beast::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
        {
            fail(ec, "open");
            return;
        }

        // Allow address reuse
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec)
        {
            fail(ec, "set_option");
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec)
        {
            fail(ec, "bind");
            return;
        }

        // Start listening for connections
        acceptor_.listen(
            net::socket_base::max_listen_connections, ec);
        if (ec)
        {
            fail(ec, "listen");
            return;
        }
    }

    // Start accepting incoming connections
    void run()
    {
        do_accept();
    }

private:
    void
        do_accept()
    {
        acceptor_.async_accept(
            net::make_strand(ioc_),
            beast::bind_front_handler(
                &listener::on_accept,
                shared_from_this()));
    }

    void
        on_accept(beast::error_code ec, tcp::socket socket)
    {
        if (ec)
        {
            fail(ec, "accept");
        }
        else
        {
            // Create the session and run it
            std::make_shared<session>(
                std::move(socket),
                doc_root_)->run();
        }

        // Accept another connection
        do_accept();
    }
};

//------------------------------------------------------------------------------
std::string getFilesDir(std::string config_path)//I'am too tired to handle exceptions here
{
    YAML::Node config = YAML::LoadFile(config_path + "/config.yaml");
    const std::string files_dir = config["files_dir"].as<std::string>();
    return files_dir;
}
int main(int argc, char* argv[])
{
    // Check command line arguments.
    if (argc != 3)
    {
        std::cerr <<
            "Wrong arguments number\n";
        return EXIT_FAILURE;
    }

	//Data initialization for socket and listener
    auto const address = net::ip::make_address("0.0.0.0");
    auto const port = 8080; //can be used by non-root
    std::string doc_root_str = getFilesDir(argv[1]);
    auto const doc_root = std::make_shared<std::string>(doc_root_str);// need shared ptr for constructor
    const int threads = 2;

	//upload directory creation
    if (!std::filesystem::is_directory(doc_root_str) ||
        !std::filesystem::exists(doc_root_str))
    {
        std::filesystem::create_directory(doc_root_str);
    }
    std::stringstream fms; //STOI throws an execptions, this thing do not lol
    fms << argv[2];
    fms >> file_maxsize;
    //

    std::thread ft1 (watchingInit, doc_root_str.c_str()); //add filewathching to the thread
    ft1.detach();
    net::io_context ioc{ threads };

	//server start
    std::make_shared<listener>(
        ioc,
        tcp::endpoint{ address, port },
        doc_root)->run();

    std::vector<std::thread> v;
    v.reserve(threads - 1);
    for (auto i = threads - 1; i > 0; --i)
        v.emplace_back(
            [&ioc]
            {
                ioc.run();
            });
    ioc.run(); //Dont really understand where is eventloop here lol

    return EXIT_SUCCESS;
}
