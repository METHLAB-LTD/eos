#pragma once

#include "common.hpp"
#include "http_session_base.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

namespace eosio { 

    //------------------------------------------------------------------------------
    // Report a failure
    void fail(beast::error_code ec, char const* what)
    {
        // ssl::error::stream_truncated, also known as an SSL "short read",
        // indicates the peer closed the connection without performing the
        // required closing handshake (for example, Google does this to
        // improve performance). Generally this can be a security issue,
        // but if your communication protocol is self-terminated (as
        // it is with both HTTP and WebSocket) then you may simply
        // ignore the lack of close_notify.
        //
        // https://github.com/boostorg/beast/issues/38
        //
        // https://security.stackexchange.com/questions/91435/how-to-handle-a-malicious-ssl-tls-shutdown
        //
        // When a short read would cut off the end of an HTTP message,
        // Beast returns the error beast::http::error::partial_message.
        // Therefore, if we see a short read here, it has occurred
        // after the message has been completed, so it is safe to ignore it.

        if(ec == asio::ssl::error::stream_truncated)
            return;

        fc_elog(logger, "${w}: ${m}", ("w", what)("m", ec.message()));
    }

    // Handle HTTP conneciton using boost::beast for TCP communication
    // This uses the Curiously Recurring Template Pattern so that
    // the same code works with both SSL streams and regular sockets.
    template<class Derived> 
    class beast_http_session : public http_session_base
    {
        protected:
            beast::flat_buffer buffer_;
            beast::error_code ec_;

            // Access the derived class, this is part of
            // the Curiously Recurring Template Pattern idiom.
            Derived& derived()
            {
                return static_cast<Derived&>(*this);
            }

            bool allow_host(const http::request<http::string_body>& req) override {
                auto is_conn_secure = is_secure();
                auto local_endpoint = derived().get_lowest_layer_endpoint();
                auto local_socket_host_port = local_endpoint.address().to_string() 
                        + ":" + std::to_string(local_endpoint.port());
                const auto& host_str = req["Host"].to_string();
                if (host_str.empty() 
                    || !host_is_valid(*plugin_state_, 
                                      host_str, 
                                      local_socket_host_port, 
                                      is_conn_secure)) 
                {
                    return false;
                }

                return true;
            }

        public: 
            // shared_from_this() requires default constuctor
            beast_http_session() = default;  
      
            // Take ownership of the buffer
            beast_http_session(
                std::shared_ptr<http_plugin_state> plugin_state,
                asio::io_context* ioc)
                : http_session_base(plugin_state, ioc)
            { }

            virtual ~beast_http_session() = default;            

            void do_read()
            {
                // just use sync reads for now - P. Raphael
                beast::error_code ec;
                auto bytes_read = http::read(derived().stream(), buffer_, req_parser_, ec);
                on_read(ec, bytes_read);                
            }

            void on_read(beast::error_code ec,
                         std::size_t bytes_transferred)
            {
                boost::ignore_unused(bytes_transferred);

                // This means they closed the connection
                if(ec == http::error::end_of_stream)
                    return derived().do_eof();

                if(ec && ec != asio::ssl::error::stream_truncated)
                    return fail(ec, "read");


                auto req = req_parser_.get();
                // Send the response
                handle_request(std::move(req));
            }

            void on_write(beast::error_code ec,
                          std::size_t bytes_transferred, 
                          bool close)
            {
                boost::ignore_unused(bytes_transferred);

                if(ec) {
                    ec_ = ec;
                    handle_exception();
                    return fail(ec, "write");
                }

                if(close)
                {
                    // This means we should close the connection, usually because
                    // the response indicated the "Connection: close" semantic.
                    return derived().do_eof();
                }

                // Read another request
                do_read();
            }           

            virtual void handle_exception() override {
                auto errCodeStr = std::to_string(ec_.value());
                fc_elog( logger, "beast_websession_exception: error code ${ec}", ("ec", errCodeStr));
            }

            virtual void send_response(std::optional<std::string> body, int code) override {
                // Determine if we should close the connection after
                bool close = !(plugin_state_->keep_alive) || res_.need_eof();

                res_.result(code);
                if(body.has_value())
                    res_.body() = *body;        

                res_.prepare_payload();

                // just use sync writes for now - P. Raphael
                beast::error_code ec;
                auto bytes_written = http::write(derived().stream(), res_, ec);
                on_write(ec, bytes_written, close);
            }
    }; // end class beast_http_session

    // Handles a plain HTTP connection
    class plain_session
        : public beast_http_session<plain_session> 
        , public std::enable_shared_from_this<plain_session>
    {

#if BOOST_VERSION < 107000     
        tcp_socket_t socket_;
        asio::strand<asio::io_context::executor_type> strand_;
#else
        beast::tcp_stream stream_;
#endif
        public:      
            // Create the session
            plain_session(
                tcp_socket_t socket,
                std::shared_ptr<ssl::context> ctx,
                std::shared_ptr<http_plugin_state> plugin_state,
                asio::io_context* ioc
                )
                : beast_http_session<plain_session>(plugin_state, ioc)
// for boost versions < 1.70, we need to create the strand and have it live inside the session                     
// for boost versions >= 1.70, this is taken care of by make_strand() call in the listener
#if BOOST_VERSION < 107000       
                , socket_(std::move(socket))
                , strand_(socket_.get_executor())
#else
                , stream_(std::move(socket))
#endif                
            {}

            // Called by the base class
#if BOOST_VERSION < 107000                   
            tcp_socket_t& stream() { return socket_; }
#else            
            beast::tcp_stream& stream() { return stream_; }
#endif            

            // Start the asynchronous operation
            void run()
            {
                // catch any loose exceptions so that nodeos will return zero exit code
                try {
                    do_read();
                } catch (fc::exception& e) {
                    fc_dlog( logger, "fc::exception thrown while invoking unix_socket_session::run()");
                    fc_dlog( logger, "Details: ${e}", ("e", e.to_detail_string()) );
                } catch (std::exception& e) {
                    fc_elog( logger, "STD exception thrown while invoking beast_http_session::run()");
                    fc_dlog( logger, "Exception Details: ${e}", ("e", e.what()) );
                } catch (...) {
                    fc_elog( logger, "Unknown exception thrown while invoking beast_http_session::run()");
                }
            }

            void do_eof()
            {
                // Send a TCP shutdown
                beast::error_code ec;

#if BOOST_VERSION < 107000                
                socket_.shutdown(tcp::socket::shutdown_send, ec);
#else                
                stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
#endif                

                // At this point the connection is closed gracefully
            }

            auto get_lowest_layer_endpoint() {
#if BOOST_VERSION < 107000                
                auto& lowest_layer = beast::get_lowest_layer<tcp_socket_t&>(socket_);
                return lowest_layer.local_endpoint();
#else
                auto& lowest_layer = beast::get_lowest_layer(stream_);
                return lowest_layer.socket().local_endpoint();
#endif                
            }

            virtual shared_ptr<detail::abstract_conn> get_shared_from_this() override {
                return shared_from_this();
            }
    }; // end class plain_session

    // Handles an SSL HTTP connection
    class ssl_session
        : public beast_http_session<ssl_session>
        , public std::enable_shared_from_this<ssl_session>
    {        
#if BOOST_VERSION < 107000        
        tcp_socket_t socket_;
        ssl::stream<tcp_socket_t&> stream_;
        asio::strand<asio::io_context::executor_type> strand_;
#else
        beast::ssl_stream<beast::tcp_stream> stream_;
#endif      
        

        public:
            // Create the session

            ssl_session(
                tcp_socket_t socket,
                std::shared_ptr<ssl::context> ctx,
                std::shared_ptr<http_plugin_state> plugin_state,
                asio::io_context* ioc)
                : beast_http_session<ssl_session>(plugin_state, ioc)
// for boost versions < 1.70, we need to create the strand and have it live inside the session                     
// for boost versions >= 1.70, this is taken care of by make_strand() call in the listener
#if BOOST_VERSION < 107000
                , strand_(socket_.get_executor())
                , socket_(std::move(socket))
                , stream_(socket_, *ctx)
#else                 
                , stream_(std::move(socket), *ctx)
#endif                
            { }


            // Called by the base class
#if BOOST_VERSION < 107000            
            ssl::stream<tcp_socket_t&> &stream() { return stream_; }
#else            
            beast::ssl_stream<beast::tcp_stream>&  stream() { return stream_; }
#endif
            // Start the asynchronous operation
            void run()
            {
                beast::error_code ec;
                stream_.handshake(ssl::stream_base::server, ec);
                on_handshake(ec);
            }

            void on_handshake(beast::error_code ec)
            {
                
                if(ec)
                    return fail(ec, "handshake");

                do_read();
            }

            void do_eof()
            {
                // Perform the SSL shutdown
                beast::error_code ec;
                stream_.shutdown(ec);
                on_shutdown(ec);
            }

            void on_shutdown(beast::error_code ec)
            {
                if(ec)
                    return fail(ec, "shutdown");

                // At this point the connection is closed gracefully
            }

            auto get_lowest_layer_endpoint() {
#if BOOST_VERSION < 107000                
                auto& lowest_layer = beast::get_lowest_layer<tcp_socket_t&>(socket_);
                return lowest_layer.local_endpoint();
#else
                auto& lowest_layer = beast::get_lowest_layer(stream_);
                return lowest_layer.socket().local_endpoint();
#endif
                
            }

            virtual bool is_secure() override { return true; }

            virtual shared_ptr<detail::abstract_conn> get_shared_from_this() override {
                return shared_from_this();
            }

            
    }; // end class ssl_session

} // end namespace


