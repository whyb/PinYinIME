// shared/unique_handle.h — RAII wrapper for Windows HANDLE
//
// Prevents handle leaks by automatically calling CloseHandle on destruction.
// Supports move semantics so handles can be transferred between scopes.
// Modeled after std::unique_ptr with a custom deleter.
//
// Usage:
//   unique_handle h(CreateEventW(...));
//   unique_handle hMap(CreateFileMappingW(...));
//   HANDLE raw = h.get();        // borrow without releasing
//   HANDLE raw = h.release();    // take ownership away
//
#pragma once
#include <windows.h>
#include <type_traits>

// ── unique_handle: owns a single HANDLE ─────────────────────────────────

class unique_handle {
public:
    unique_handle() noexcept : m_handle(nullptr), m_owns(false) {}
    explicit unique_handle(HANDLE h) noexcept : m_handle(h), m_owns(true) {}

    ~unique_handle() { close(); }

    // Non-copyable
    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    // Movable
    unique_handle(unique_handle&& other) noexcept
        : m_handle(other.m_handle), m_owns(other.m_owns) {
        other.m_handle = nullptr;
        other.m_owns = false;
    }

    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) {
            close();
            m_handle = other.m_handle;
            m_owns = other.m_owns;
            other.m_handle = nullptr;
            other.m_owns = false;
        }
        return *this;
    }

    // Release ownership without closing — caller becomes responsible
    HANDLE release() noexcept {
        HANDLE h = m_handle;
        m_handle = nullptr;
        m_owns = false;
        return h;
    }

    // Reset with a new handle (closes the old one)
    void reset(HANDLE h = nullptr) noexcept {
        close();
        m_handle = h;
        m_owns = (h != nullptr && h != INVALID_HANDLE_VALUE);
    }

    // Borrow without releasing ownership
    HANDLE get() const noexcept { return m_handle; }

    // Implicit conversion for passing to Windows APIs
    operator HANDLE() const noexcept { return m_handle; }

    // Check validity
    bool valid() const noexcept {
        return m_owns && m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }
    explicit operator bool() const noexcept { return valid(); }

    // Compare with raw HANDLE
    bool operator==(HANDLE other) const noexcept { return m_handle == other; }
    bool operator!=(HANDLE other) const noexcept { return m_handle != other; }

private:
    void close() noexcept {
        if (m_owns && m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
        m_handle = nullptr;
        m_owns = false;
    }

    HANDLE m_handle;
    bool   m_owns;
};

// ── scoped_unmap: RAII wrapper for MapViewOfFile ────────────────────────

class scoped_unmap {
public:
    scoped_unmap() noexcept : m_ptr(nullptr) {}
    explicit scoped_unmap(void* ptr) noexcept : m_ptr(ptr) {}

    ~scoped_unmap() { unmap(); }

    scoped_unmap(const scoped_unmap&) = delete;
    scoped_unmap& operator=(const scoped_unmap&) = delete;

    scoped_unmap(scoped_unmap&& other) noexcept : m_ptr(other.m_ptr) {
        other.m_ptr = nullptr;
    }

    scoped_unmap& operator=(scoped_unmap&& other) noexcept {
        if (this != &other) {
            unmap();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    void* release() noexcept { void* p = m_ptr; m_ptr = nullptr; return p; }
    void reset(void* ptr = nullptr) noexcept { unmap(); m_ptr = ptr; }
    void* get() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

private:
    void unmap() noexcept {
        if (m_ptr) {
            UnmapViewOfFile(m_ptr);
            m_ptr = nullptr;
        }
    }
    void* m_ptr;
};

// ── scoped_hkey: RAII wrapper for registry HKEY ─────────────────────────

class scoped_hkey {
public:
    scoped_hkey() noexcept : m_key(nullptr) {}
    explicit scoped_hkey(HKEY key) noexcept : m_key(key) {}

    ~scoped_hkey() { close(); }

    scoped_hkey(const scoped_hkey&) = delete;
    scoped_hkey& operator=(const scoped_hkey&) = delete;

    scoped_hkey(scoped_hkey&& other) noexcept : m_key(other.m_key) {
        other.m_key = nullptr;
    }

    scoped_hkey& operator=(scoped_hkey&& other) noexcept {
        if (this != &other) { close(); m_key = other.m_key; other.m_key = nullptr; }
        return *this;
    }

    HKEY release() noexcept { HKEY k = m_key; m_key = nullptr; return k; }
    void reset(HKEY key = nullptr) noexcept { close(); m_key = key; }
    HKEY get() const noexcept { return m_key; }
    explicit operator bool() const noexcept { return m_key != nullptr; }

private:
    void close() noexcept {
        if (m_key) { RegCloseKey(m_key); m_key = nullptr; }
    }
    HKEY m_key;
};
