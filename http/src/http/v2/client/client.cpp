// Copyright (C) 2013, 2014 by Glyn Matthews
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <boost/asio/strand.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/algorithm/find_first_of.hpp>
#include <network/uri.hpp>
#include <network/config.hpp>
#include <network/http/v2/client/client.hpp>
#include <network/http/v2/method.hpp>
#include <network/http/v2/client/request.hpp>
#include <network/http/v2/client/response.hpp>
#include <network/http/v2/client/connection/tcp_resolver.hpp>
#include <network/http/v2/client/connection/normal_connection.hpp>

namespace network {
  namespace http {
    namespace v2 {
      using boost::asio::ip::tcp;

      struct request_context {

        std::shared_ptr<client_connection::async_connection> connection_;

        request request_;
        request_options options_;

        std::promise<response> response_promise_;

        boost::asio::streambuf request_buffer_;
        boost::asio::streambuf response_buffer_;

        std::uint64_t total_bytes_written_, total_bytes_read_;

        request_context(
            std::shared_ptr<client_connection::async_connection> connection,
            request request, request_options options)
            : connection_(connection),
              request_(request),
              options_(options),
              total_bytes_written_(0),
              total_bytes_read_(0) {}
      };

      struct client::impl {

        explicit impl(client_options options);

        impl(std::unique_ptr<client_connection::async_resolver> mock_resolver,
             std::unique_ptr<client_connection::async_connection>
                 mock_connection, client_options options);

        ~impl();

        void set_error(const boost::system::error_code &ec,
                       std::shared_ptr<request_context> context);

        std::future<response> execute(std::shared_ptr<request_context> context);

        void timeout(const boost::system::error_code &ec,
                     std::shared_ptr<request_context> context);

        void connect(const boost::system::error_code &ec,
                     tcp::resolver::iterator endpoint_iterator,
                     std::shared_ptr<request_context> context);

        void write_request(const boost::system::error_code &ec,
                           std::shared_ptr<request_context> context);

        void write_body(const boost::system::error_code &ec,
                        std::size_t bytes_written,
                        std::shared_ptr<request_context> context);

        void read_response(const boost::system::error_code &ec,
                           std::size_t bytes_written,
                           std::shared_ptr<request_context> context);

        void read_response_status(const boost::system::error_code &ec,
                                  std::size_t bytes_written,
                                  std::shared_ptr<request_context> context);

        void read_response_headers(const boost::system::error_code &ec,
                                   std::size_t bytes_read,
                                   std::shared_ptr<request_context> context,
                                   std::shared_ptr<response> res);

        void read_response_body(const boost::system::error_code &ec,
                                std::size_t bytes_read,
                                std::shared_ptr<request_context> context,
                                std::shared_ptr<response> res);

        client_options options_;
        boost::asio::io_service io_service_;
        std::unique_ptr<boost::asio::io_service::work> sentinel_;
        boost::asio::io_service::strand strand_;
        std::unique_ptr<client_connection::async_resolver> resolver_;
        std::shared_ptr<client_connection::async_connection> mock_connection_;
        bool timedout_;
        boost::asio::deadline_timer timer_;
        std::thread lifetime_thread_;

      };

      client::impl::impl(client_options options)
          : options_(options),
            sentinel_(new boost::asio::io_service::work(io_service_)),
            strand_(io_service_),
            resolver_(new client_connection::tcp_resolver(
                io_service_, options_.cache_resolved())),
            timedout_(false),
            timer_(io_service_),
            lifetime_thread_([=]() { io_service_.run(); }) {}

      client::impl::impl(
          std::unique_ptr<client_connection::async_resolver> mock_resolver,
          std::unique_ptr<client_connection::async_connection> mock_connection,
          client_options options)
          : options_(options),
            sentinel_(new boost::asio::io_service::work(io_service_)),
            strand_(io_service_),
            resolver_(std::move(mock_resolver)),
            timedout_(false),
            timer_(io_service_),
            lifetime_thread_([=]() { io_service_.run(); }) {}

      client::impl::~impl() {
        sentinel_.reset();
        lifetime_thread_.join();
      }


      void client::impl::set_error(const boost::system::error_code &ec,
                                   std::shared_ptr<request_context> context) {
        context->response_promise_.set_exception(std::make_exception_ptr(
            std::system_error(ec.value(), std::system_category())));
        timer_.cancel();
      }

      std::future<response> client::impl::execute(
          std::shared_ptr<request_context> context) {
        std::future<response> res = context->response_promise_.get_future();

        // If there is no user-agent, provide one as a default.
        auto user_agent = context->request_.header("User-Agent");
        if (!user_agent) {
          context->request_.append_header("User-Agent", options_.user_agent());
        }

        // Get the host and port from the request and resolve
        auto url = context->request_.url();
        auto host = url.host() ? uri::string_type(std::begin(*url.host()),
                                                  std::end(*url.host()))
                               : uri::string_type();
        auto port = url.port<std::uint16_t>() ? *url.port<std::uint16_t>() : 80;

        resolver_->async_resolve(
            host, port,
            strand_.wrap([=](const boost::system::error_code &ec,
                             tcp::resolver::iterator endpoint_iterator) {
              connect(ec, endpoint_iterator, context);
            }));

        if (options_.timeout() > std::chrono::milliseconds(0)) {
          timer_.expires_from_now(boost::posix_time::milliseconds(options_.timeout().count()));
          timer_.async_wait(strand_.wrap([=](const boost::system::error_code &ec) {
                timeout(ec, context);
              }));
        }

        return res;
      }

      void client::impl::timeout(const boost::system::error_code &ec,
                                 std::shared_ptr<request_context> context) {
        if (!ec) {
          context->connection_->disconnect();
        }
        timedout_ = true;
      }

      void client::impl::connect(const boost::system::error_code &ec,
                                 tcp::resolver::iterator endpoint_iterator,
                                 std::shared_ptr<request_context> context) {
        if (ec) {
          set_error(ec, context);
          return;
        }

        // make a connection to an endpoint
        auto host = context->request_.url().host();
        tcp::endpoint endpoint(*endpoint_iterator);
        context->connection_->async_connect(
            endpoint, std::string(std::begin(*host), std::end(*host)),
            strand_.wrap([=](const boost::system::error_code &ec) {
              // If there is no connection, try again on another endpoint
              if (ec && endpoint_iterator != tcp::resolver::iterator()) {
                // copy iterator because it is const after the lambda
                // capture
                auto it = endpoint_iterator;
                boost::system::error_code ignore;
                connect(ignore, ++it, context);
                return;
              }

              write_request(ec, context);
            }));
      }

      void client::impl::write_request(
          const boost::system::error_code &ec,
          std::shared_ptr<request_context> context) {
        if (timedout_) {
          set_error(boost::asio::error::timed_out, context);
          return;
        }

        if (ec) {
          set_error(ec, context);
          return;
        }

        // write the request to an I/O stream.
        std::ostream request_stream(&context->request_buffer_);
        request_stream << context->request_;
        if (!request_stream) {
          context->response_promise_.set_exception(std::make_exception_ptr(
              client_exception(client_error::invalid_request)));
          timer_.cancel();
        }

        context->connection_->async_write(
            context->request_buffer_,
            strand_.wrap([=](const boost::system::error_code &ec,
                             std::size_t bytes_written) {
              write_body(ec, bytes_written, context);
            }));
      }

      void client::impl::write_body(const boost::system::error_code &ec,
                                    std::size_t bytes_written,
                                    std::shared_ptr<request_context> context) {
        if (timedout_) {
          set_error(boost::asio::error::timed_out, context);
          return;
        }

        if (ec) {
          set_error(ec, context);
          return;
        }

        // update progress
        context->total_bytes_written_ += bytes_written;
        if (auto progress = context->options_.progress()) {
          progress(client_message::transfer_direction::bytes_written,
                   context->total_bytes_written_);
        }

        // write the body to an I/O stream
        std::ostream request_stream(&context->request_buffer_);
        // TODO write payload to request_buffer_
        if (!request_stream) {
          context->response_promise_.set_exception(std::make_exception_ptr(
              client_exception(client_error::invalid_request)));
        }

        context->connection_->async_write(
            context->request_buffer_,
            strand_.wrap([=](const boost::system::error_code &ec,
                             std::size_t bytes_written) {
              read_response(ec, bytes_written, context);
            }));
      }

      void client::impl::read_response(
          const boost::system::error_code &ec, std::size_t bytes_written,
          std::shared_ptr<request_context> context) {
        if (timedout_) {
          set_error(boost::asio::error::timed_out, context);
          return;
        }

        if (ec) {
          set_error(ec, context);
          return;
        }

        // update progress.
        context->total_bytes_written_ += bytes_written;
        if (auto progress = context->options_.progress()) {
          progress(client_message::transfer_direction::bytes_written,
                   context->total_bytes_written_);
        }

        // Create a response object and fill it with the status from the server.
        context->connection_->async_read_until(
            context->response_buffer_, "\r\n",
            strand_.wrap([=](const boost::system::error_code &ec,
                             std::size_t bytes_read) {
              read_response_status(ec, bytes_read, context);
            }));
      }

      void client::impl::read_response_status(
          const boost::system::error_code &ec, std::size_t,
          std::shared_ptr<request_context> context) {
        if (timedout_) {
          set_error(boost::asio::error::timed_out, context);
          return;
        }

        if (ec) {
          set_error(ec, context);
          return;
        }

        // Update the reponse status.
        std::istream is(&context->response_buffer_);
        string_type version;
        is >> version;
        unsigned int status;
        is >> status;
        string_type message;
        std::getline(is, message);

        // if options_.follow_redirects()
        // and if status in range 300 - 307
        // then take the request and reset the URL from the 'Location' header
        // restart connection
        // Not that the 'Location' header can be a *relative* URI

        std::shared_ptr<response> res(new response{});
        res->set_version(version);
        res->set_status(network::http::status::code(status));
        res->set_status_message(boost::trim_copy(message));

        // Read the response headers.
        context->connection_->async_read_until(
            context->response_buffer_, "\r\n\r\n",
            strand_.wrap([=](const boost::system::error_code &ec,
                             std::size_t bytes_read) {
              read_response_headers(ec, bytes_read, context, res);
            }));
      }

      void client::impl::read_response_headers(
          const boost::system::error_code &ec, std::size_t,
          std::shared_ptr<request_context> context,
          std::shared_ptr<response> res) {
        if (timedout_) {
          set_error(boost::asio::error::timed_out, context);
          return;
        }

        if (ec) {
          set_error(ec, context);
          return;
        }

        // fill headers
        std::istream is(&context->response_buffer_);
        string_type header;
        while (std::getline(is, header) && (header != "\r")) {
          auto delim = boost::find_first_of(header, ":");
          string_type key(std::begin(header), delim);
          while (*++delim == ' ') {
          }
          string_type value(delim, std::end(header));
          res->add_header(key, value);
        }

        // read the response body.
        context->connection_->async_read(
            context->response_buffer_,
            strand_.wrap([=](const boost::system::error_code &ec,
                             std::size_t bytes_read) {
              read_response_body(ec, bytes_read, context, res);
            }));
      }

      namespace {
        // I don't want to to delimit with newlines when using the input
        // stream operator, so that's why I wrote this function.
        std::istream &getline_with_newline(std::istream &is,
                                           std::string &line) {
          line.clear();

          std::istream::sentry se(is, true);
          std::streambuf *sb = is.rdbuf();

          while (true) {
            int c = sb->sbumpc();
            switch (c) {
              case EOF:
                if (line.empty()) {
                  is.setstate(std::ios::eofbit);
                }
                return is;
              default:
                line += static_cast<char>(c);
            }
          }
        }
      }  // namespace

      void client::impl::read_response_body(
          const boost::system::error_code &ec, std::size_t bytes_read,
          std::shared_ptr<request_context> context,
          std::shared_ptr<response> res) {
        if (timedout_) {
          set_error(boost::asio::error::timed_out, context);
          return;
        }

        if (ec && ec != boost::asio::error::eof) {
          set_error(ec, context);
          return;
        }

        // update progress.
        context->total_bytes_read_ += bytes_read;
        if (auto progress = context->options_.progress()) {
          progress(client_message::transfer_direction::bytes_read,
                   context->total_bytes_read_);
        }

        // If there's no data else to read, then set the response and exit.
        if (bytes_read == 0) {
          context->response_promise_.set_value(*res);
          timer_.cancel();
          return;
        }

        std::istream is(&context->response_buffer_);
        string_type line;
        line.reserve(bytes_read);
        while (!getline_with_newline(is, line).eof()) {
          res->append_body(std::move(line));
        }

        // Keep reading the response body until we have nothing else to read.
        context->connection_->async_read(
            context->response_buffer_,
            strand_.wrap([=](const boost::system::error_code &ec,
                             std::size_t bytes_read) {
              read_response_body(ec, bytes_read, context, res);
            }));
      }

      client::client(client_options options) : pimpl_(new impl(options)) {}

      client::client(
          std::unique_ptr<client_connection::async_resolver> mock_resolver,
          std::unique_ptr<client_connection::async_connection> mock_connection,
          client_options options)
          : pimpl_(new impl(std::move(mock_resolver),
                            std::move(mock_connection), options)) {}

      client::~client() { }

      std::future<response> client::execute(request req,
                                            request_options options) {
        std::shared_ptr<client_connection::async_connection> connection;
        if (pimpl_->mock_connection_) {
          connection = pimpl_->mock_connection_;
        } else {
          // TODO factory based on HTTP or HTTPS
          connection = std::make_shared<client_connection::normal_connection>(
              pimpl_->io_service_);
        }
        return pimpl_->execute(
            std::make_shared<request_context>(connection, req, options));
      }

      std::future<response> client::get(request req, request_options options) {
        req.method(method::get);
        return execute(req, options);
      }

      std::future<response> client::post(request req, request_options options) {
        req.method(method::post);
        return execute(req, options);
      }

      std::future<response> client::put(request req, request_options options) {
        req.method(method::put);
        return execute(req, options);
      }

      std::future<response> client::delete_(request req,
                                            request_options options) {
        req.method(method::delete_);
        return execute(req, options);
      }

      std::future<response> client::head(request req, request_options options) {
        req.method(method::head);
        return execute(req, options);
      }

      std::future<response> client::options(request req,
                                            request_options options) {
        req.method(method::options);
        return execute(req, options);
      }
    }  // namespace v2
  }    // namespace http
}  // namespace network
