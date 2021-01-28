#include "commandline.h"
#include <functional>
#include <mutex>

#if defined(__linux) || defined(__linux__)
#include <pthread.h>
#include <stdio.h>
#include <termios.h>
#endif

#include <iostream>

Commandline::Commandline()
    : m_io_thread(std::bind(&Commandline::io_thread_main, this)) {
}

Commandline::~Commandline() {
    m_shutdown.store(true);
    m_io_thread.join();
}

// we want to get a char without echoing it to the terminal and without buffering.
// this is platform specific, so multiple different implementations are defined below.
static char getchar_no_echo();

// for windows, we use _getch
#if defined(_WIN32)
static char getchar_no_echo() {
    return _getch();
}
#elif defined(__linux) || defined(__linux__)
// for linux, we take care of non-echoing and non-buffered input with termios
namespace detail {
static struct termios s_old_termios, s_current_termios;

static void init_termios(bool echo) {
    tcgetattr(0, &s_old_termios);
    s_current_termios = s_old_termios;
    s_current_termios.c_lflag &= ~ICANON;
    if (echo) {
        s_current_termios.c_lflag |= ECHO;
    } else {
        s_current_termios.c_lflag &= ~ECHO;
    }
    tcsetattr(0, TCSANOW, &s_current_termios);
}

static void reset_termios() {
    tcsetattr(0, TCSANOW, &s_old_termios);
}

static char getch_(bool echo) {
    char ch;
    init_termios(echo);
    ch = getchar();
    reset_termios();
    return ch;
}
} // namespace detail

static char getchar_no_echo() {
    return detail::getch_(false);
}
#else // any other OS needs to #define either __linux or WIN32, or implement their own.
#error "Please choose __linux or WIN32 implementation of `getchar_no_echo`, or implement your own"
#endif // WIN32, __linux

void Commandline::add_to_current_buffer(char c) {
    m_current_buffer += c;
    ++m_cursor_pos;
    putchar(c);
    m_history_temp_buffer = m_current_buffer;
}

void Commandline::update_current_buffer_view() {
    printf("\x1b[2K\x1b[1000D%s", m_current_buffer.c_str());
    fflush(stdout);
}

void Commandline::handle_escape_sequence() {
    char c2 = getchar_no_echo();
    char c3 = getchar_no_echo();
    if (c2 == '[' && history_enabled()) {
        if (c3 == 'A' && !m_history.empty()) {
            go_back_in_history();
            std::lock_guard<std::mutex> guard_history(m_history_mutex);
            if (m_history_index == m_history.size()) {
                m_current_buffer = m_history_temp_buffer;
            } else {
                m_current_buffer = m_history.at(m_history_index);
            }
            update_current_buffer_view();
        } else if (c3 == 'B' && !m_history.empty()) {
            go_forward_in_history();
            std::lock_guard<std::mutex> guard_history(m_history_mutex);
            if (m_history_index == m_history.size()) {
                m_current_buffer = m_history_temp_buffer;
            } else {
                m_current_buffer = m_history.at(m_history_index);
            }
            update_current_buffer_view();
        }
    } else {
        add_to_current_buffer(c2);
        add_to_current_buffer(c3);
    }
}

void Commandline::handle_backspace() {
    if (!m_current_buffer.empty()) {
        --m_cursor_pos;
        m_current_buffer.pop_back();
        update_current_buffer_view();
    }
}

void Commandline::input_thread_main() {
    while (!m_shutdown.load()) {
        char c = 0;
        while (c != '\n' && !m_shutdown.load()) {
            c = getchar_no_echo();
            std::lock_guard<std::mutex> guard(m_current_buffer_mutex);
            if (c == '\b' || c == 127) { // backspace or other delete sequence
                handle_backspace();
            } else if (isprint(c)) {
                add_to_current_buffer(c);
            } else if (c == 0x1b) { // escape sequence
                handle_escape_sequence();
            }
        }
        // check so we dont do anything on the last pass before exit
        if (!m_shutdown.load()) {
            if (history_enabled()) {
                add_to_history(m_current_buffer);
            }
            std::lock_guard<std::mutex> guard(m_to_read_mutex);
            m_to_read.push(m_current_buffer);
            m_current_buffer.clear();
            m_cursor_pos = 0;
        }
    }
}

void Commandline::io_thread_main() {
    std::thread input_thread(&Commandline::input_thread_main, this);
    while (!m_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard<std::mutex> guard(m_to_write_mutex);
        if (m_to_write.empty()) {
            continue;
        } else {
            auto to_write = m_to_write.front();
            m_to_write.pop();
            printf("\x1b[2K\x1b[1000D%s\n", to_write.c_str());
            std::lock_guard<std::mutex> guard2(m_current_buffer_mutex);
            printf("%s", m_current_buffer.c_str());
            fflush(stdout);
        }
    }
#if !defined(WIN32)
    // on POSIX systems, we can cancel the pthread in order to terminate it gracefully
    // without leaking resources
    pthread_cancel(input_thread.native_handle());
    input_thread.join();
#else
    // .. on non-posix systems, we just detach it, until we find a better way to timeout the input in the input thread.
    i_thread.detach();
#endif
    // after all this, we have to output all that remains in the buffer, so we dont "lose" information
    while (!m_to_write.empty()) {
        auto to_write = m_to_write.front();
        m_to_write.pop();
        printf("\x1b[2K\x1b[1000D%s\n", to_write.c_str());
    }
    fflush(stdout);
}

void Commandline::add_to_history(const std::string& str) {
    std::lock_guard<std::mutex> guard(m_history_mutex);
    // if adding one entry would put us over the limit,
    // remove the oldest one before adding one
    if (m_history.size() + 1 > m_history_limit) {
        m_history.erase(m_history.begin());
    }
    m_history.push_back(str);
    m_history_index = m_history.size(); // point to one after last one
    m_history_temp_buffer.clear();
}

void Commandline::go_back_in_history() {
    std::lock_guard<std::mutex> guard(m_history_mutex);
    if (m_history_index == 0) {
        return;
    } else {
        --m_history_index;
    }
}

void Commandline::go_forward_in_history() {
    std::lock_guard<std::mutex> guard(m_history_mutex);
    if (m_history_index == m_history.size()) {
        return;
    } else {
        ++m_history_index;
    }
}

void Commandline::write(const std::string& str) {
    std::lock_guard<std::mutex> guard(m_to_write_mutex);
    m_to_write.push(str);
}

bool Commandline::has_command() const {
    std::lock_guard<std::mutex> guard(m_to_read_mutex);
    return !m_to_read.empty();
}

std::string Commandline::get_command() {
    std::lock_guard<std::mutex> guard(m_to_read_mutex);
    auto res = m_to_read.front();
    m_to_read.pop();
    return res;
}

void Commandline::set_history_limit(size_t count) {
    std::lock_guard<std::mutex> guard(m_history_mutex);
    m_history_limit = count;
}

size_t Commandline::history_size() const {
    std::lock_guard<std::mutex> guard(m_history_mutex);
    return m_history.size();
}

void Commandline::clear_history() {
    std::lock_guard<std::mutex> guard(m_history_mutex);
    m_history.clear();
}
