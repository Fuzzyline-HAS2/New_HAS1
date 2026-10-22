/**
 * HTTPClient.cpp
 *
 * Created on: 02.11.2015
 *
 * Copyright (c) 2015 Markus Sattler. All rights reserved.
 * This file is part of the HTTPClient for Arduino.
 * Port to ESP32 by Evandro Luis Copercini (2017),
 * changed fingerprints to CA verification.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Adapted in October 2018
 */

// Scoped Arduino-ESP32 3.3.11 response parser; see UPSTREAM.md.
int HAS2BoundedHTTP::writeToStream(Stream *stream) {

  if (!stream) {
    return returnError(HTTPC_ERROR_NO_STREAM);
  }

  if (_size == 0 && _transferEncoding == HTTPC_TE_IDENTITY) return 0;
  if (!connected()) {
    if (_size == -1 && _transferEncoding == HTTPC_TE_IDENTITY) return 0;
    return returnError(HTTPC_ERROR_NOT_CONNECTED);
  }

  // get length of document (is -1 when Server sends no Content-Length header)
  int len = _size;
  int ret = 0;

  if (_transferEncoding == HTTPC_TE_IDENTITY) {
    ret = writeToStreamDataBlock(stream, len);

    // have we an error?
    if (ret < 0) {
      return returnError(ret);
    }
  } else if (_transferEncoding == HTTPC_TE_CHUNKED) {
    int size = 0;
    while (1) {
      if (!connected()) {
        return returnError(HTTPC_ERROR_CONNECTION_LOST);
      }
      String chunkHeader;
      if (!line(chunkHeader, 128)) return HTTPC_ERROR_READ_TIMEOUT;

      if (chunkHeader.length() <= 0) {
        return returnError(HTTPC_ERROR_READ_TIMEOUT);
      }

      chunkHeader.trim();  // remove \r

      // read size of chunk
      char *chunkEnd = nullptr;
      if (!isxdigit(static_cast<unsigned char>(chunkHeader[0]))) { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
      const unsigned long chunkSize = strtoul(chunkHeader.c_str(), &chunkEnd, 16);
      if (!chunkEnd || (*chunkEnd && *chunkEnd != ';')) { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
      if (chunkSize > 8192 || size > 8192 - static_cast<int>(chunkSize)) { socket_.fail(HAS2HttpError::TooLarge); return HTTPC_ERROR_ENCODING; }
      len = static_cast<int>(chunkSize);
      size += len;
      log_v(" read chunk len: %d", len);

      // data left?
      if (len > 0) {
        int r = writeToStreamDataBlock(stream, len);
        if (r < 0) {
          // error in writeToStreamDataBlock
          return returnError(r);
        }
        ret += r;
      } else {

        // if no length Header use global chunk size
        if (_size <= 0) {
          _size = size;
        }

        // check if we have write all data out
        if (ret != _size) {
          return returnError(HTTPC_ERROR_STREAM_WRITE);
        }
        // A zero chunk is followed by trailers and the final empty CRLF line.
        size_t trailerBytes = 0;
        while (true) {
          String trailer;
          if (!line(trailer, 1024)) return HTTPC_ERROR_READ_TIMEOUT;
          trailerBytes += trailer.length() + 2;
          if (trailerBytes > 2048) { socket_.fail(HAS2HttpError::TooLarge); return HTTPC_ERROR_ENCODING; }
          if (!trailer.length()) break;
          if (trailer.indexOf(':') <= 0) { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
        }
        break;
      }

      // read trailing \r\n at the end of the chunk
      char buf[2];
      auto trailing_seq_len = _client->readBytes((uint8_t *)buf, 2);
      if (trailing_seq_len != 2 || buf[0] != '\r' || buf[1] != '\n') {
        return returnError(HTTPC_ERROR_READ_TIMEOUT);
      }

      delay(0);
    }
  } else {
    return returnError(HTTPC_ERROR_ENCODING);
  }

  //    end();
  // Do not call SDK disconnect(): its RX clear loop can drain forever.
  socket_.stop();
  return ret;
}


int HAS2BoundedHTTP::handleHeaderResponse() {

  if (!connected()) {
    return HTTPC_ERROR_NOT_CONNECTED;
  }

  _returnCode = 0;
  _size = -1;
  _canReuse = _reuse;

  String transferEncoding;

  _transferEncoding = HTTPC_TE_IDENTITY;
  unsigned long lastDataTime = millis();
  bool firstLine = true;
  String date;
  size_t headerBytes = 0;
  bool contentLengthSeen = false;

  while (connected()) {
    size_t len = _client->available();
    if (len > 0) {
      String headerLine;
      if (!line(headerLine, 1024)) return HTTPC_ERROR_READ_TIMEOUT;
      headerBytes += headerLine.length() + 2;
      if (headerBytes > 4096) { socket_.fail(HAS2HttpError::TooLarge); return HTTPC_ERROR_ENCODING; }
      headerLine.trim();  // remove \r

      lastDataTime = millis();

      log_v("RX: '%s'", headerLine.c_str());

      if (firstLine) {
        firstLine = false;
        if (_canReuse && headerLine.startsWith("HTTP/1.")) {
          _canReuse = (headerLine[sizeof "HTTP/1." - 1] != '0');
        }
        if (!headerLine.startsWith("HTTP/1.0 ") && !headerLine.startsWith("HTTP/1.1 ")) { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
        int codePos = headerLine.indexOf(' ') + 1;
        if (headerLine.length() < static_cast<size_t>(codePos + 3) ||
            headerLine[codePos] < '0' || headerLine[codePos] > '9' ||
            headerLine[codePos + 1] < '0' || headerLine[codePos + 1] > '9' ||
            headerLine[codePos + 2] < '0' || headerLine[codePos + 2] > '9' ||
            (headerLine.length() > static_cast<size_t>(codePos + 3) && headerLine[codePos + 3] != ' ')) {
          socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING;
        }
        _returnCode = headerLine.substring(codePos, codePos + 3).toInt();
        if (_returnCode < 200 || _returnCode > 599) { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
      } else if (headerLine.length()) {
        if (headerLine.indexOf(':') <= 0) { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
        String headerName = headerLine.substring(0, headerLine.indexOf(':'));
        String headerValue = headerLine.substring(headerLine.indexOf(':') + 1);
        headerValue.trim();

        if (headerName.equalsIgnoreCase("Date")) {
          date = headerValue;
        }

        if (headerName.equalsIgnoreCase("Content-Length")) {
          if (contentLengthSeen || !headerValue.length()) { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
          contentLengthSeen = true;
          uint32_t length = 0;
          for (size_t i = 0; i < headerValue.length(); ++i) {
            if (headerValue[i] < '0' || headerValue[i] > '9') { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
            length = length > 8192 ? 8193 : length * 10 + headerValue[i] - '0';
            if (length > 8192 && _returnCode == 200) { socket_.fail(HAS2HttpError::TooLarge); return HTTPC_ERROR_ENCODING; }
          }
          _size = length;
        }

        if (_canReuse && headerName.equalsIgnoreCase("Connection")) {
          if (headerValue.indexOf("close") >= 0 && headerValue.indexOf("keep-alive") < 0) {
            _canReuse = false;
          }
        }

        if (headerName.equalsIgnoreCase("Transfer-Encoding")) {
          if (transferEncoding.length()) { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
          transferEncoding = headerValue;
        }

        if (headerName.equalsIgnoreCase("Location")) {
          _location = headerValue;
        }

        if (headerName.equalsIgnoreCase("Set-Cookie")) {
          setCookie(date, headerValue);
        }

        if (_collectAllHeaders && headerName.length() > 0) {
          _currentHeaders.push_back({headerName, headerValue});
        } else {
          for (size_t i = 0; i < _currentHeaders.size(); ++i) {
            if (_currentHeaders[i].key.equalsIgnoreCase(headerName)) {
              _currentHeaders[i].value = headerValue;
              break;  // We found a match, stop looking
            }
          }
        }
      }

      if (headerLine == "") {
        log_d("code: %d", _returnCode);

        if (_size > 0) {
          log_d("size: %d", _size);
        }

        if (transferEncoding.length() && contentLengthSeen) { socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING; }
        if (transferEncoding.length() > 0) {
          log_d("Transfer-Encoding: %s", transferEncoding.c_str());
          if (transferEncoding.equalsIgnoreCase("chunked")) {
            _transferEncoding = HTTPC_TE_CHUNKED;
          } else if (transferEncoding.equalsIgnoreCase("identity")) {
            _transferEncoding = HTTPC_TE_IDENTITY;
          } else {
            socket_.fail(HAS2HttpError::Framing); return HTTPC_ERROR_ENCODING;
          }
        } else {
          _transferEncoding = HTTPC_TE_IDENTITY;
        }

        if (_returnCode) {
          return _returnCode;
        } else {
          log_d("Remote host is not an HTTP Server!");
          return HTTPC_ERROR_NO_HTTP_SERVER;
        }
      }

    } else {
      if ((millis() - lastDataTime) > _tcpTimeout) {
        return HTTPC_ERROR_READ_TIMEOUT;
      }
      delay(10);
    }
  }

  return HTTPC_ERROR_CONNECTION_LOST;
}


int HAS2BoundedHTTP::writeToStreamDataBlock(Stream *stream, int size) {
  int buff_size = HTTP_TCP_RX_BUFFER_SIZE;
  int len = size;
  int bytesWritten = 0;

  // if possible create smaller buffer then HTTP_TCP_RX_BUFFER_SIZE
  if ((len > 0) && (len < buff_size)) {
    buff_size = len;
  }

  // create buffer for read
  uint8_t *buff = (uint8_t *)malloc(buff_size);

  if (buff) {
    // read all data from server
    while (connected() && (len > 0 || len == -1)) {

      // get available data size
      size_t sizeAvailable = buff_size;
      if (len < 0) {
        sizeAvailable = _client->available();
      }

      if (sizeAvailable) {

        int readBytes = sizeAvailable;

        // read only the asked bytes
        if (len > 0 && readBytes > len) {
          readBytes = len;
        }

        // not read more the buffer can handle
        if (readBytes > buff_size) {
          readBytes = buff_size;
        }

        // stop if no more reading
        if (readBytes == 0) {
          break;
        }

        // read data
        int bytesRead = _client->readBytes(buff, readBytes);
        if (bytesRead == 0) { free(buff); socket_.fail(HAS2HttpError::Transport); return HTTPC_ERROR_READ_TIMEOUT; }

        // write it to Stream
        int bytesWrite = stream->write(buff, bytesRead);
        bytesWritten += bytesWrite;

        // are all Bytes a written to stream ?
        if (bytesWrite != bytesRead) {
          log_d("short write asked for %d but got %d retry...", bytesRead, bytesWrite);

          // check for write error
          if (stream->getWriteError()) {
            log_d("stream write error %d", stream->getWriteError());

            //reset write error for retry
            stream->clearWriteError();
          }

          // some time for the stream
          delay(1);

          int leftBytes = (bytesRead - bytesWrite);

          // retry to send the missed bytes
          bytesWrite = stream->write((buff + bytesWrite), leftBytes);
          bytesWritten += bytesWrite;

          if (bytesWrite != leftBytes) {
            // failed again
            log_w("short write asked for %d but got %d failed.", leftBytes, bytesWrite);
            free(buff);
            return HTTPC_ERROR_STREAM_WRITE;
          }
        }

        // check for write error
        if (stream->getWriteError()) {
          log_w("stream write error %d", stream->getWriteError());
          free(buff);
          return HTTPC_ERROR_STREAM_WRITE;
        }

        // count bytes to read left
        if (len > 0) {
          len -= bytesRead;
        }

        delay(0);
      } else {
        delay(1);
      }
    }

    free(buff);

    log_v("connection closed or file end (written: %d).", bytesWritten);

    if ((size > 0) && (size != bytesWritten)) {
      log_d("bytesWritten %d and size %d mismatch!.", bytesWritten, size);
      socket_.fail(HAS2HttpError::Transport);
      return HTTPC_ERROR_STREAM_WRITE;
    }

  } else {
    log_w("too less ram! need %d", buff_size);
    return HTTPC_ERROR_TOO_LESS_RAM;
  }

  return bytesWritten;
}

