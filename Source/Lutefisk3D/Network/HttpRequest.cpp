//
// Copyright (c) 2008-2016 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Network/HttpRequest.h"
#include "../IO/Log.h"
#include "../Core/Profiler.h"
#include "../Core/Timer.h"

#include <Civetweb/civetweb.h>

#include "../DebugNew.h"

namespace Urho3D
{

static const unsigned ERROR_BUFFER_SIZE = 256;
static const unsigned READ_BUFFER_SIZE = 65536; // Must be a power of two

HttpRequest::HttpRequest(const QString& url, const QString& verb, const std::vector<QString> & headers, const QString& postData) :
    url_(url.trimmed()),
    verb_(!verb.isEmpty() ? verb : "GET"),
    headers_(headers),
    postData_(postData),
    state_(HTTP_INITIALIZING),
    httpReadBuffer_(new unsigned char[READ_BUFFER_SIZE]),
    readBuffer_(new unsigned char[READ_BUFFER_SIZE]),
    readPosition_(0),
    writePosition_(0)
{
    // Size of response is unknown, so just set maximum value. The position will also be changed
    // to maximum value once the request is done, signaling end for Deserializer::IsEof().
    size_ = M_MAX_UNSIGNED;

    URHO3D_LOGDEBUG("HTTP " + verb_ + " request to URL " + url_);

    // Start the worker thread to actually create the connection and read the response data.
    Run();
}

HttpRequest::~HttpRequest()
{
    Stop();
}

void HttpRequest::ThreadFunction()
{
    QString protocol = "http";
    QString host;
    QString path = "/";
    int port = 80;

    int protocolEnd = url_.indexOf("://");
    if (protocolEnd != -1)
    {
        protocol = url_.mid(0, protocolEnd);
        host = url_.mid(protocolEnd + 3);
    }
    else
        host = url_;

    int pathStart = host.indexOf('/');
    if (pathStart != -1)
    {
        path = host.mid(pathStart);
        host = host.mid(0, pathStart);
    }

    int portStart = host.indexOf(':');
    if (portStart != -1)
    {
        port = ToInt(host.mid(portStart + 1));
        host = host.mid(0, portStart);
    }

    char errorBuffer[ERROR_BUFFER_SIZE];
    memset(errorBuffer, 0, sizeof(errorBuffer));

    QString headersStr;
    for (unsigned i = 0; i < headers_.size(); ++i)
    {
        // Trim and only add non-empty header strings
        QString header = headers_[i].trimmed();
        if (header.length())
            headersStr += header + "\r\n";
    }

    // Initiate the connection. This may block due to DNS query
    /// \todo SSL mode will not actually work unless Civetweb's SSL mode is initialized with an external SSL DLL
    mg_connection* connection = nullptr;
    if (postData_.isEmpty())
    {
        connection = mg_download(qPrintable(host), port, protocol.compare("https", Qt::CaseInsensitive) ? 0 : 1, errorBuffer, sizeof(errorBuffer),
            "%s %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "%s"
            "\r\n", qPrintable(verb_), qPrintable(path), qPrintable(host), qPrintable(headersStr));
    }
    else
    {
        connection = mg_download(qPrintable(host), port, protocol.compare("https", Qt::CaseInsensitive) ? 0 : 1, errorBuffer, sizeof(errorBuffer),
            "%s %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "%s"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s", qPrintable(verb_),qPrintable(path), qPrintable(host), qPrintable(headersStr), postData_.length(), qPrintable(postData_));
    }

    {
        MutexLock lock(mutex_);
        state_ = connection ? HTTP_OPEN : HTTP_ERROR;

        // If no connection could be made, store the error and exit
        if (state_ == HTTP_ERROR)
        {
            error_ = QString(&errorBuffer[0]);
            return;
        }
    }

    // Loop while should run, read data from the connection, copy to the main thread buffer if there is space
    while (shouldRun_)
    {
        // Read less than full buffer to be able to distinguish between full and empty ring buffer. Reading may block
        int bytesRead = mg_read(connection, httpReadBuffer_.Get(), READ_BUFFER_SIZE / 4);
        if (bytesRead <= 0)
            break;

        mutex_.Acquire();

        // Wait until enough space in the main thread's ring buffer
        for (;;)
        {
            unsigned spaceInBuffer = READ_BUFFER_SIZE - ((writePosition_ - readPosition_) & (READ_BUFFER_SIZE - 1));
            if ((int)spaceInBuffer > bytesRead || !shouldRun_)
                break;

            mutex_.Release();
            Time::Sleep(5);
            mutex_.Acquire();
        }

        if (!shouldRun_)
        {
            mutex_.Release();
            break;
        }

        if (writePosition_ + bytesRead <= READ_BUFFER_SIZE)
            memcpy(readBuffer_.Get() + writePosition_, httpReadBuffer_.Get(), bytesRead);
        else
        {
            // Handle ring buffer wrap
            unsigned part1 = READ_BUFFER_SIZE - writePosition_;
            unsigned part2 = bytesRead - part1;
            memcpy(readBuffer_.Get() + writePosition_, httpReadBuffer_.Get(), part1);
            memcpy(readBuffer_.Get(), httpReadBuffer_.Get() + part1, part2);
        }

        writePosition_ += bytesRead;
        writePosition_ &= READ_BUFFER_SIZE - 1;

        mutex_.Release();
    }

    // Close the connection
    mg_close_connection(connection);

    {
        MutexLock lock(mutex_);
        state_ = HTTP_CLOSED;
    }
}

unsigned HttpRequest::Read(void* dest, unsigned size)
{
    mutex_.Acquire();

    unsigned char* destPtr = (unsigned char*)dest;
    unsigned sizeLeft = size;
    unsigned totalRead = 0;

    for (;;)
    {
        unsigned bytesAvailable;

        for (;;)
        {
            bytesAvailable = CheckEofAndAvailableSize();
            if (bytesAvailable || IsEof())
                break;
            // While no bytes and connection is still open, block until has some data
            mutex_.Release();
            Time::Sleep(5);
            mutex_.Acquire();
        }

        if (bytesAvailable)
        {
            if (bytesAvailable > sizeLeft)
                bytesAvailable = sizeLeft;

            if (readPosition_ + bytesAvailable <= READ_BUFFER_SIZE)
                memcpy(destPtr, readBuffer_.Get() + readPosition_, bytesAvailable);
            else
            {
                // Handle ring buffer wrap
                unsigned part1 = READ_BUFFER_SIZE - readPosition_;
                unsigned part2 = bytesAvailable - part1;
                memcpy(destPtr, readBuffer_.Get() + readPosition_, part1);
                memcpy(destPtr + part1, readBuffer_.Get(), part2);
            }

            readPosition_ += bytesAvailable;
            readPosition_ &= READ_BUFFER_SIZE - 1;
            sizeLeft -= bytesAvailable;
            totalRead += bytesAvailable;
            destPtr += bytesAvailable;
        }

        if (!sizeLeft || !bytesAvailable)
            break;
    }

    // Check for end-of-file once more after reading the bytes
    CheckEofAndAvailableSize();
    mutex_.Release();
    return totalRead;
}

unsigned HttpRequest::Seek(unsigned position)
{
    return position_;
}

QString HttpRequest::GetError() const
{
    MutexLock lock(mutex_);
    const_cast<HttpRequest*>(this)->CheckEofAndAvailableSize();
    return error_;
}

HttpRequestState HttpRequest::GetState() const
{
    MutexLock lock(mutex_);
    const_cast<HttpRequest*>(this)->CheckEofAndAvailableSize();
    return state_;
}

unsigned HttpRequest::GetAvailableSize() const
{
    MutexLock lock(mutex_);
    return const_cast<HttpRequest*>(this)->CheckEofAndAvailableSize();
}

unsigned HttpRequest::CheckEofAndAvailableSize()
{
    unsigned bytesAvailable = (writePosition_ - readPosition_) & (READ_BUFFER_SIZE - 1);
    if (state_ == HTTP_ERROR || (state_ == HTTP_CLOSED && !bytesAvailable))
        position_ = M_MAX_UNSIGNED;
    return bytesAvailable;
}

}