#include <sentinel/client.h>

#include <sentinel/proto.h>

#include <sstream>
#include <vector>

#include <cassert>

namespace sentinel::client {

namespace {

auto
to_handle(uv_tcp_t* handle) -> uv_handle_t*
{
  return reinterpret_cast<uv_handle_t*>(handle);
}

auto
to_handle(uv_stream_t* handle) -> uv_handle_t*
{
  return reinterpret_cast<uv_handle_t*>(handle);
}

auto
to_handle(uv_connect_t* handle) -> uv_handle_t*
{
  return reinterpret_cast<uv_handle_t*>(handle);
}

auto
to_handle(uv_write_t* handle) -> uv_handle_t*
{
  return reinterpret_cast<uv_handle_t*>(handle);
}

auto
to_handle(uv_signal_t* handle) -> uv_handle_t*
{
  return reinterpret_cast<uv_handle_t*>(handle);
}

class write_operation final
{
public:
  using complete_cb = void (*)(void*, bool success);

  static void send(uv_stream_t* socket, std::vector<std::uint8_t> data, void* cb_data, complete_cb cb_func)
  {
    auto* op = new write_operation(std::move(data), cb_data, cb_func);

    if (!op->send(socket)) {
      if (cb_func) {
        cb_func(cb_data, false);
      }
      delete op;
    }
  }

protected:
  write_operation(std::vector<std::uint8_t> data, void* cb_data, complete_cb cb_func)
    : m_data(std::move(data))
    , m_cb_data(cb_data)
    , m_cb_func(cb_func)
  {
    m_buffer.base = reinterpret_cast<char*>(&m_data[0]);
    m_buffer.len = m_data.size();

    uv_handle_set_data(to_handle(&m_handle), this);
  }

  auto send(uv_stream_t* socket) -> bool { return uv_write(&m_handle, socket, &m_buffer, 1, on_write_complete) == 0; }

  static void on_write_complete(uv_write_t* handle, int status)
  {
    auto* self = static_cast<write_operation*>(uv_handle_get_data(to_handle(handle)));

    if (self->m_cb_func) {
      self->m_cb_func(self->m_cb_data, status == 0);
    }

    delete self;
  }

private:
  std::vector<std::uint8_t> m_data;

  uv_buf_t m_buffer{};

  uv_write_t m_handle{};

  void* m_cb_data{ nullptr };

  complete_cb m_cb_func{ nullptr };
};

class connection_impl final : public connection
{
public:
  connection_impl(uv_loop_t* loop, bool interrupt_handling)
  {
    uv_handle_set_data(to_handle(&m_socket), this);

    uv_tcp_init(loop, &m_socket);

    uv_handle_set_data(to_handle(&m_signal_handler), this);

    uv_signal_init(loop, &m_signal_handler);

    if (interrupt_handling) {

      uv_signal_start(&m_signal_handler, on_interrupt, SIGINT);
    }
  }

  ~connection_impl() { assert(m_closed == true); }

  void connect(const char* ip, int port) override
  {
    if (m_is_connecting) {
      notify_error("Cannot attempt more than one connection at a time.");
      return;
    }

    sockaddr_in address{};

    if (uv_ip4_addr(ip, port, &address) != 0) {
      notify_error("Failed to parse IPv4 address.");
      return;
    }

    auto* handle = new uv_connect_t;

    uv_handle_set_data(to_handle(handle), this);

    if (uv_tcp_connect(handle, &m_socket, reinterpret_cast<const sockaddr*>(&address), on_connect) != 0) {
      notify_error("Failed to initiate connection.");
      delete handle;
      return;
    }

    m_is_connecting = true;
  }

  void close() override
  {
    if (m_closed) {
      return;
    }

    uv_close(to_handle(&m_socket), on_close);

    uv_close(to_handle(&m_signal_handler), nullptr);

    m_closed = true;
  }

  void add_observer(observer* o) override { m_observers.emplace_back(std::move(o)); }

  void notify_ready() override
  {
    proto::writer w("ready", 0, /* conflate */ true);

    write_operation::send(
      reinterpret_cast<uv_stream_t*>(&m_socket), std::move(*w.complete()->buffer), nullptr, nullptr);
  }

  void set_streaming_enabled(const bool enabled) override { m_streaming_enabled = enabled; }

  auto caught_interrupt() const -> bool override { return m_caught_interrupt; }

protected:
  static auto get_self(uv_handle_t* handle) -> connection_impl*
  {
    return static_cast<connection_impl*>(uv_handle_get_data(handle));
  }

  static void on_interrupt(uv_signal_t* signal, int signum)
  {
    auto* handle = to_handle(signal);

    auto* loop = uv_handle_get_loop(handle);

    auto* self = get_self(handle);

    if (signum == SIGINT) {

      self->m_caught_interrupt = true;

      uv_stop(loop);
    }
  }

  static void on_close(uv_handle_t* handle)
  {
    auto* self = get_self(handle);

    for (auto* obs : self->m_observers) {
      obs->on_connection_closed();
    }

    self->m_closed = true;
  }

  static void on_connect(uv_connect_t* handle, int status)
  {
    auto* self = get_self(to_handle(handle));

    delete handle;

    self->m_is_connecting = false;

    if (status != 0) {
      for (auto* obs : self->m_observers) {
        obs->on_connection_failed();
      }
      return;
    }

    for (auto* obs : self->m_observers) {
      obs->on_connection_established();
    }

    if (uv_read_start(reinterpret_cast<uv_stream_t*>(&self->m_socket), on_alloc, on_read) != 0) {
      self->notify_error("Failed to start reading from server.");
      return;
    }
  }

  static void on_alloc(uv_handle_t* handle, const size_t size, uv_buf_t* buf)
  {
    auto* self = get_self(handle);

    self->m_read_buffer.resize(self->m_read_size + size);

    buf->base = reinterpret_cast<char*>(self->m_read_buffer.data() + self->m_read_size);

    buf->len = size;
  }

  static void on_read(uv_stream_t* stream, ssize_t read_size, const uv_buf_t* buf)
  {
    auto* self = get_self(to_handle(stream));

    if (read_size < 0) {
      self->notify_error("Failed to read message from server.");
      return;
    }

    self->m_read_size += static_cast<std::size_t>(read_size);

    self->attempt_read_message();
  }

  void attempt_read_message()
  {
    const auto result = proto::read(m_read_buffer.data(), m_read_size);

    if (result.payload_ready) {
      handle_message(result);
    }

    m_read_buffer.erase(m_read_buffer.begin(), m_read_buffer.begin() + result.cull_size);

    m_read_size -= result.cull_size;
  }

  void handle_message(const proto::read_result& r)
  {
    for (auto* o : m_observers) {
      o->on_payload(r.type_id, m_read_buffer.data() + r.payload_offset, r.payload_size);
    }

    if (m_streaming_enabled) {
      notify_ready();
    }
  }

  void notify_error(const char* what)
  {
    for (auto* obs : m_observers) {
      obs->on_error(what);
    }
  }

private:
  uv_tcp_t m_socket{};

  uv_signal_t m_signal_handler{};

  std::vector<observer*> m_observers;

  bool m_closed{ false };

  bool m_is_connecting{ false };

  std::vector<std::uint8_t> m_read_buffer;

  std::size_t m_read_size{ 0 };

  bool m_streaming_enabled{ true };

  bool m_caught_interrupt{ false };
};

} // namespace

auto
connection::create(uv_loop_t* loop, bool interrupt_handling) -> std::unique_ptr<connection>
{
  return std::make_unique<connection_impl>(loop, interrupt_handling);
}

} // namespace sentinel::client
