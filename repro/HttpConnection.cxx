
#include <cassert>

#include "resiprocate/os/Data.hxx"
#include "resiprocate/os/Socket.hxx"
#include "resiprocate/Symbols.hxx"
#include "resiprocate/os/TransportType.hxx"
#include "resiprocate/os/Logger.hxx"
#include "resiprocate/os/Tuple.hxx"
#include "resiprocate/os/DnsUtil.hxx"
#include "resiprocate/os/ParseBuffer.hxx"

#include "repro/HttpBase.hxx"
#include "repro/HttpConnection.hxx"


using namespace resip;
using namespace repro;
using namespace std;

#define RESIPROCATE_SUBSYSTEM Subsystem::REPRO

int HttpConnection::nextPageNumber=1;




HttpConnection::HttpConnection( HttpBase& base, Socket pSock ):
   mHttpBase( base ),
   mPageNumber(nextPageNumber++),
   mSock(pSock),
   mParsedRequest(false)
{
}


HttpConnection::~HttpConnection()
{
   close(mSock); mSock=0;
}

      
void 
HttpConnection::buildFdSet(FdSet& fdset)
{
   if ( !mTxBuffer.empty() )
   {
      fdset.setWrite(mSock);
   }
   fdset.setRead(mSock);
}


bool 
HttpConnection::process(FdSet& fdset)
{
   if ( fdset.hasException(mSock) )
   {
      int errNum = 0;
      int errNumSize = sizeof(errNum);
      getsockopt(mSock,SOL_SOCKET,SO_ERROR,(char *)&errNum,(socklen_t *)&errNumSize);
      InfoLog (<< "Exception reading from socket " 
               << mSock << " code: " << errNum << "; closing connection");
      return false;
   }
   
   if ( fdset.readyToRead( mSock ) )
   {
      bool ok = processSomeReads();
      if ( !ok )
      {
         return false;
      }
   }
   if ( (!mTxBuffer.empty()) && fdset.readyToWrite( mSock ) )
   {
      bool ok = processSomeWrites();
      if ( !ok )
      {
         return false;
      }
   }

   return true;
}


void 
HttpConnection::setPage(const Data& page)
{
   if ( page.empty() )
   {
      mTxBuffer += "HTTP/1.0 301 Moved Permanently"; mTxBuffer += Symbols::CRLF;
      mTxBuffer += "Location: http:/index.html"; mTxBuffer += Symbols::CRLF;

      page = Data ( "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">"
                    "<html><head>"
                    "<title>301 Moved Permanently</title>"
                    "</head><body>"
                    "<h1>Moved</h1>"
                    "</body></html>" );
      
   }
   else
   {
      mTxBuffer += "HTTP/1.0 200 OK" ; mTxBuffer += Symbols::CRLF;
   }
   
   Data len;
   {
      DataStream s(len);
      s << page.size();
      s.flush();
   }
   
   mTxBuffer += "Server: Repro Proxy " ; mTxBuffer += Symbols::CRLF;
   mTxBuffer += "Content-Length: "; mTxBuffer += len; mTxBuffer += Symbols::CRLF;
   mTxBuffer += "Content-Type: text/html" ; mTxBuffer += Symbols::CRLF;
   mTxBuffer += Symbols::CRLF;
   
   mTxBuffer += page;
}


bool
HttpConnection::processSomeReads()
{
   const int bufSize = 8000;
   char buf[bufSize];
   
 
#if defined(WIN32)
   int bytesRead = ::recv(mSock, buf, bufSize, 0);
#else
   int bytesRead = ::read(mSock, buf, bufSize);
#endif

   if (bytesRead == INVALID_SOCKET)
   {
      int e = getErrno();
      switch (e)
      {
         case EAGAIN:
            InfoLog (<< "No data ready to read");
            return true;
         case EINTR:
            InfoLog (<< "The call was interrupted by a signal before any data was read.");
            break;
         case EIO:
            InfoLog (<< "I/O error");
            break;
         case EBADF:
            InfoLog (<< "fd is not a valid file descriptor or is not open for reading.");
            break;
         case EINVAL:
            InfoLog (<< "fd is attached to an object which is unsuitable for reading.");
            break;
         case EFAULT:
            InfoLog (<< "buf is outside your accessible address space.");
            break;
         default:
            InfoLog (<< "Some other error");
            break;
      }
      InfoLog (<< "Failed read on " << mSock << " " << strerror(e));
      return false;
   }
   else if (bytesRead == 0)
   {
      InfoLog (<< "Connection closed by remote " );
      return false;
   }

   //DebugLog (<< "HttpConnection::processSomeReads() " 
   //          << " read=" << bytesRead);            

   mRxBuffer += Data( buf, bytesRead );
   
   tryParse();
   
   return true;
}


void 
HttpConnection::tryParse()
{
   //DebugLog (<< "parse " << mRxBuffer );
   
   ParseBuffer pb(mRxBuffer);
   
   pb.skipToChar(Symbols::SPACE[0]);
   const char* start = pb.skipWhitespace();
   pb.skipToChar(Symbols::SPACE[0]);
   
   if (pb.eof())
   {
      // parse failed - just return 
      return;
   }
   
   Data uri;
   pb.data( uri, start );
 
   DebugLog (<< "parse found URI " << uri );
   mParsedRequest = true;
      
   mHttpBase.buildPage(uri,mPageNumber);
}


bool
HttpConnection::processSomeWrites()
{
   if ( mTxBuffer.empty() )
   {
      return true;
   }
   
   //DebugLog (<< "Writing " << mTxBuffer );

#if defined(WIN32)
   int bytesWritten = ::send( mSock, mTxBuffer.data(), mTxBuffer.size(), 0);
#else
   int bytesWritten = ::write(mSock, mTxBuffer.data(), mTxBuffer.size() );
#endif

   if (bytesWritten == INVALID_SOCKET)
   {
      int e = getErrno();
      InfoLog (<< "HttpConnection failed write on " << mSock << " " << strerror(e));

      return false;
   }
   
   if (bytesWritten == (int)mTxBuffer.size() )
   {
      DebugLog (<< "Wrote it all" );
      mTxBuffer = Data::Empty;

      return false; // return false causes connection to close and clean up
   }
   else
   {
      Data rest = mTxBuffer.substr(bytesWritten);
      mTxBuffer = rest;
      DebugLog( << "Wrote " << bytesWritten << " bytes - still need to do " << mTxBuffer );
   }
   
   return true;
}


/* ====================================================================
 * The Vovida Software License, Version 1.0 
 * 
 * Copyright (c) 2000 Vovida Networks, Inc.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 * 
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * 
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by Vovida
 * Networks, Inc. and many individuals on behalf of Vovida Networks,
 * Inc.  For more information on Vovida Networks, Inc., please see
 * <http://www.vovida.org/>.
 *
 */
