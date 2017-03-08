/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef INCLUDED_SSLSOCKET_HPP
#define INCLUDED_SSLSOCKET_HPP

#include <cerrno>

#include "Ssl.hpp"
#include "Socket.hpp"

/// An SSL/TSL, non-blocking, data streaming socket.
class SslStreamSocket : public StreamSocket
{
public:
    SslStreamSocket(const int fd, std::unique_ptr<SocketHandlerInterface> responseClient) :
        StreamSocket(fd, std::move(responseClient)),
        _ssl(nullptr),
        _sslWantsTo(SslWantsTo::Neither),
        _doHandshake(true)
    {
        LOG_DBG("SslStreamSocket ctor #" << fd);

        BIO* bio = BIO_new(BIO_s_socket());
        if (bio == nullptr)
        {
            throw std::runtime_error("Failed to create SSL BIO.");
        }

        BIO_set_fd(bio, fd, BIO_NOCLOSE);

        _ssl = SslContext::newSsl();
        if (!_ssl)
        {
            BIO_free(bio);
            throw std::runtime_error("Failed to create SSL.");
        }

        SSL_set_bio(_ssl, bio, bio);

        // We are a server-side socket.
        SSL_set_accept_state(_ssl);
    }

    ~SslStreamSocket()
    {
        LOG_DBG("SslStreamSocket dtor #" << getFD());

        shutdown();
        SSL_free(_ssl);
    }

    /// Shutdown the TLS/SSL connection properly.
    void shutdown() override
    {
        if (SSL_shutdown(_ssl) == 0)
        {
            // Complete the bidirectional shutdown.
            SSL_shutdown(_ssl);
        }
    }

    bool readIncomingData() override
    {
        assert(isCorrectThread());

        const int rc = doHandshake();
        if (rc <= 0)
        {
            return (rc != 0);
        }

        // Default implementation.
        return StreamSocket::readIncomingData();
    }

    void writeOutgoingData() override
    {
        assert(isCorrectThread());

        const int rc = doHandshake();
        if (rc <= 0)
        {
            return;
        }

        // Default implementation.
        StreamSocket::writeOutgoingData();
    }

    virtual int readData(char* buf, int len)
    {
        assert(isCorrectThread());

        return handleSslState(SSL_read(_ssl, buf, len));
    }

    virtual int writeData(const char* buf, const int len)
    {
        assert(isCorrectThread());

        assert (len > 0); // Never write 0 bytes.
        return handleSslState(SSL_write(_ssl, buf, len));
    }

    int getPollEvents() override
    {
        if (_sslWantsTo == SslWantsTo::Read)
        {
            // Must read next before attempting to write.
            return POLLIN;
        }
        else if (_sslWantsTo == SslWantsTo::Write)
        {
            // Must write next before attempting to read.
            return POLLOUT;
        }

        // Do the default.
        return StreamSocket::getPollEvents();
    }

private:

    /// The possible next I/O operation that SSL want to do.
    enum class SslWantsTo
    {
        Neither,
        Read,
        Write
    };

    int doHandshake()
    {
        assert(isCorrectThread());

        if (_doHandshake)
        {
            int rc;
            do
            {
                rc = SSL_do_handshake(_ssl);
            }
            while (rc < 0 && errno == EINTR);

            if (rc <= 0)
            {
                rc = handleSslState(rc);
                if (rc <= 0)
                {
                    return (rc != 0);
                }
            }

            _doHandshake = false;
        }

        // Handshake complete.
        return 1;
    }

    /// Handles the state of SSL after read or write.
    int handleSslState(const int rc)
    {
        assert(isCorrectThread());

        if (rc > 0)
        {
            // Success: Reset so we can do either.
            _sslWantsTo = SslWantsTo::Neither;
            return rc;
        }

        // Last operation failed. Find out if SSL was trying
        // to do something different that failed, or not.
        const int sslError = SSL_get_error(_ssl, rc);
        LOG_TRC("SSL error: " << sslError);
        switch (sslError)
        {
        case SSL_ERROR_ZERO_RETURN:
            // Shutdown complete, we're disconnected.
            return 0;

        case SSL_ERROR_WANT_READ:
            _sslWantsTo = SslWantsTo::Read;
            return rc;

        case SSL_ERROR_WANT_WRITE:
            _sslWantsTo = SslWantsTo::Write;
            return rc;

        case SSL_ERROR_WANT_CONNECT:
        case SSL_ERROR_WANT_ACCEPT:
        case SSL_ERROR_WANT_X509_LOOKUP:
            // Unexpected.
            return rc;

        case SSL_ERROR_SYSCALL:
            if (errno != 0)
            {
                // Posix API error, let the caller handle.
                return rc;
            }

            // Fallthrough...
        default:
            {
                // The error is comming from BIO. Find out what happened.
                const long bioError = ERR_get_error();
                LOG_TRC("BIO error: " << bioError);
                if (bioError == 0)
                {
                    if (rc == 0)
                    {
                        // Socket closed.
                        return 0;
                    }
                    else if (rc == -1)
                    {
                        throw std::runtime_error("SSL Socket closed unexpectedly.");
                    }
                    else
                    {
                        throw std::runtime_error("SSL BIO reported error [" + std::to_string(rc) + "].");
                    }
                }
                else
                {
                    char buf[512];
                    ERR_error_string_n(bioError, buf, sizeof(buf));
                    throw std::runtime_error(buf);
                }
            }
            break;
        }

        return rc;
    }

private:
    SSL* _ssl;
    /// During handshake SSL might want to read
    /// on write, or write on read.
    SslWantsTo _sslWantsTo;
    /// We must do the handshake during the first
    /// read or write in non-blocking.
    bool _doHandshake;
};

#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
