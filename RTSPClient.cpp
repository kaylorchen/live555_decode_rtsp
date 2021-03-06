/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// Copyright (c) 1996-2020, Live Networks, Inc.  All rights reserved
// A demo application, showing how to create and run a RTSP client (that can potentially receive multiple streams concurrently).
//
// NOTE: This code - although it builds a running application - is intended only to illustrate how to develop your own RTSP
// client application.  For a full-featured RTSP client application - with much more functionality, and many options - see
// "openRTSP": http://www.live555.com/openRTSP/

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "iostream"
#include <SDL_rect.h>
#include <SDL_render.h>
#include <SDL.h>



extern "C"
{
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "linux/videodev2.h"
#include "sys/mman.h"
}

AVCodec *codec;
AVCodecContext *c = NULL;
AVFrame *frame;
AVCodecParserContext *avParserContext;
bool fHaveWrittenFirstFrame = false;

SDL_Window *sdlWindow;
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
SDL_Rect sdlRect;
bool SDLInit = false;
AVFrame *pFrameYUV;
struct SwsContext *img_convert_ctx = NULL;

// Forward function definitions:
void decode_init(void);
void sdl_init(unsigned int width, unsigned int height);
int decoderyuv(unsigned char *inbuf, int read_size);
void sdl_stop(void);
static int get_char();
// int decoderyuv(unsigned char * inbuf, int read_size)
// RTSP 'response handlers':
void continueAfterDESCRIBE(RTSPClient *rtspClient, int resultCode, char *resultString);
void continueAfterSETUP(RTSPClient *rtspClient, int resultCode, char *resultString);
void continueAfterPLAY(RTSPClient *rtspClient, int resultCode, char *resultString);

// Other event handler functions:
void subsessionAfterPlaying(void *clientData); // called when a stream's subsession (e.g., audio or video substream) ends
void subsessionByeHandler(void *clientData, char const *reason);
// called when a RTCP "BYE" is received for a subsession
void streamTimerHandler(void *clientData);
// called at the end of a stream's expected duration (if the stream has not already signaled its end using a RTCP "BYE")

// The main streaming routine (for each "rtsp://" URL):
void openURL(UsageEnvironment &env, char const *progName, char const *rtspURL);

// Used to iterate through each stream's 'subsessions', setting up each one:
void setupNextSubsession(RTSPClient *rtspClient);

// Used to shut down and close a stream (including its "RTSPClient" object):
void shutdownStream(RTSPClient *rtspClient, int exitCode = 1);

// A function that outputs a string that identifies each stream (for debugging output).  Modify this if you wish:
UsageEnvironment &operator<<(UsageEnvironment &env, const RTSPClient &rtspClient)
{
  return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).  Modify this if you wish:
UsageEnvironment &operator<<(UsageEnvironment &env, const MediaSubsession &subsession)
{
  return env << subsession.mediumName() << "/" << subsession.codecName();
}

void usage(UsageEnvironment &env, char const *progName)
{
  env << "Usage: " << progName << " <rtsp-url-1> ... <rtsp-url-N>\n";
  env << "\t(where each <rtsp-url-i> is a \"rtsp://\" URL)\n";
}

char eventLoopWatchVariable = 0;

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("Please type: ./RTSPClient URL1 URL2 URL3 ...\n");
    return 1;
  }
  // Begin by setting up our usage environment:
  TaskScheduler *scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment *env = BasicUsageEnvironment::createNew(*scheduler);

  // We need at least one "rtsp://" URL argument:
  // if (argc < 2) {
  //   usage(*env, argv[0]);
  //   return 1;
  // }

  // There are argc-1 URLs: argv[1] through argv[argc-1].  Open and start streaming each one:
  for (int i = 1; i <= argc - 1; ++i)
  {
    openURL(*env, argv[0], argv[i]);
  }
  // openURL(*env, argv[0], "rtsp://192.168.15.160:8554/h264Live");

  decode_init();

  // All subsequent activity takes place within the event loop:
  env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
  // This function call does not return, unless, at some point in time, "eventLoopWatchVariable" gets set to something non-zero.

  return 0;

  // If you choose to continue the application past this point (i.e., if you comment out the "return 0;" statement above),
  // and if you don't intend to do anything more with the "TaskScheduler" and "UsageEnvironment" objects,
  // then you can also reclaim the (small) memory used by these objects by uncommenting the following code:
  /*
    env->reclaim(); env = NULL;
    delete scheduler; scheduler = NULL;
  */
}

// Define a class to hold per-stream state that we maintain throughout each stream's lifetime:

class StreamClientState
{
public:
  StreamClientState();
  virtual ~StreamClientState();

public:
  MediaSubsessionIterator *iter;
  MediaSession *session;
  MediaSubsession *subsession;
  TaskToken streamTimerTask;
  double duration;
};

// If you're streaming just a single stream (i.e., just from a single URL, once), then you can define and use just a single
// "StreamClientState" structure, as a global variable in your application.  However, because - in this demo application - we're
// showing how to play multiple streams, concurrently, we can't do that.  Instead, we have to have a separate "StreamClientState"
// structure for each "RTSPClient".  To do this, we subclass "RTSPClient", and add a "StreamClientState" field to the subclass:

class ourRTSPClient : public RTSPClient
{
public:
  static ourRTSPClient *createNew(UsageEnvironment &env, char const *rtspURL,
                                  int verbosityLevel = 0,
                                  char const *applicationName = NULL,
                                  portNumBits tunnelOverHTTPPortNum = 0);

protected:
  ourRTSPClient(UsageEnvironment &env, char const *rtspURL,
                int verbosityLevel, char const *applicationName, portNumBits tunnelOverHTTPPortNum);
  // called only by createNew();
  virtual ~ourRTSPClient();

public:
  StreamClientState scs;
};

// Define a data sink (a subclass of "MediaSink") to receive the data for each subsession (i.e., each audio or video 'substream').
// In practice, this might be a class (or a chain of classes) that decodes and then renders the incoming audio or video.
// Or it might be a "FileSink", for outputting the received data into a file (as is done by the "openRTSP" application).
// In this example code, however, we define a simple 'dummy' sink that receives incoming data, but does nothing with it.

class DummySink : public MediaSink
{
public:
  static DummySink *createNew(UsageEnvironment &env,
                              MediaSubsession &subsession,  // identifies the kind of data that's being received
                              char const *streamId = NULL); // identifies the stream itself (optional)

private:
  DummySink(UsageEnvironment &env, MediaSubsession &subsession, char const *streamId);
  // called only by "createNew()"
  virtual ~DummySink();

  static void afterGettingFrame(void *clientData, unsigned frameSize,
                                unsigned numTruncatedBytes,
                                struct timeval presentationTime,
                                unsigned durationInMicroseconds);
  void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
                         struct timeval presentationTime, unsigned durationInMicroseconds);

private:
  // redefined virtual functions:
  virtual Boolean continuePlaying();

private:
  u_int8_t *fReceiveBuffer;
  MediaSubsession &fSubsession;
  char *fStreamId;
};

#define RTSP_CLIENT_VERBOSITY_LEVEL 1 // by default, print verbose output from each "RTSPClient"

static unsigned rtspClientCount = 0; // Counts how many streams (i.e., "RTSPClient"s) are currently in use.

void openURL(UsageEnvironment &env, char const *progName, char const *rtspURL)
{
  // Begin by creating a "RTSPClient" object.  Note that there is a separate "RTSPClient" object for each stream that we wish
  // to receive (even if more than stream uses the same "rtsp://" URL).
  RTSPClient *rtspClient = ourRTSPClient::createNew(env, rtspURL, RTSP_CLIENT_VERBOSITY_LEVEL, progName);
  if (rtspClient == NULL)
  {
    env << "Failed to create a RTSP client for URL \"" << rtspURL << "\": " << env.getResultMsg() << "\n";
    return;
  }

  ++rtspClientCount;

  // Next, send a RTSP "DESCRIBE" command, to get a SDP description for the stream.
  // Note that this command - like all RTSP commands - is sent asynchronously; we do not block, waiting for a response.
  // Instead, the following function call returns immediately, and we handle the RTSP response later, from within the event loop:
  rtspClient->sendDescribeCommand(continueAfterDESCRIBE);
}

// Implementation of the RTSP 'response handlers':

void continueAfterDESCRIBE(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  do
  {
    UsageEnvironment &env = rtspClient->envir();                 // alias
    StreamClientState &scs = ((ourRTSPClient *)rtspClient)->scs; // alias

    if (resultCode != 0)
    {
      env << *rtspClient << "Failed to get a SDP description: " << resultString << "\n";
      delete[] resultString;
      break;
    }

    char *const sdpDescription = resultString;
    env << *rtspClient << "Got a SDP description:\n"
        << sdpDescription << "\n";

    // Create a media session object from this SDP description:
    scs.session = MediaSession::createNew(env, sdpDescription);
    delete[] sdpDescription; // because we don't need it anymore
    if (scs.session == NULL)
    {
      env << *rtspClient << "Failed to create a MediaSession object from the SDP description: " << env.getResultMsg() << "\n";
      break;
    }
    else if (!scs.session->hasSubsessions())
    {
      env << *rtspClient << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
      break;
    }

    // Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
    // calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
    // (Each 'subsession' will have its own data source.)
    scs.iter = new MediaSubsessionIterator(*scs.session);
    setupNextSubsession(rtspClient);
    return;
  } while (0);

  // An unrecoverable error occurred with this stream.
  shutdownStream(rtspClient);
}

// By default, we request that the server stream its data using RTP/UDP.
// If, instead, you want to request that the server stream via RTP-over-TCP, change the following to True:
#define REQUEST_STREAMING_OVER_TCP False

void setupNextSubsession(RTSPClient *rtspClient)
{
  UsageEnvironment &env = rtspClient->envir();                 // alias
  StreamClientState &scs = ((ourRTSPClient *)rtspClient)->scs; // alias

  scs.subsession = scs.iter->next();
  if (scs.subsession != NULL)
  {
    if (!scs.subsession->initiate())
    {
      env << *rtspClient << "Failed to initiate the \"" << *scs.subsession << "\" subsession: " << env.getResultMsg() << "\n";
      setupNextSubsession(rtspClient); // give up on this subsession; go to the next one
    }
    else
    {
      env << *rtspClient << "Initiated the \"" << *scs.subsession << "\" subsession (";
      if (scs.subsession->rtcpIsMuxed())
      {
        env << "client port " << scs.subsession->clientPortNum();
      }
      else
      {
        env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum() + 1;
      }
      env << ")\n";

      // Continue setting up this subsession, by sending a RTSP "SETUP" command:
      rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP);
    }
    return;
  }

  // We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
  if (scs.session->absStartTime() != NULL)
  {
    // Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
  }
  else
  {
    scs.duration = scs.session->playEndTime() - scs.session->playStartTime();
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY);
  }
}

void continueAfterSETUP(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  do
  {
    UsageEnvironment &env = rtspClient->envir();                 // alias
    StreamClientState &scs = ((ourRTSPClient *)rtspClient)->scs; // alias

    if (resultCode != 0)
    {
      env << *rtspClient << "Failed to set up the \"" << *scs.subsession << "\" subsession: " << resultString << "\n";
      break;
    }

    env << *rtspClient << "Set up the \"" << *scs.subsession << "\" subsession (";
    if (scs.subsession->rtcpIsMuxed())
    {
      env << "client port " << scs.subsession->clientPortNum();
    }
    else
    {
      env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum() + 1;
    }
    env << ")\n";

    // Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
    // (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
    // after we've sent a RTSP "PLAY" command.)

    scs.subsession->sink = DummySink::createNew(env, *scs.subsession, rtspClient->url());
    // perhaps use your own custom "MediaSink" subclass instead
    if (scs.subsession->sink == NULL)
    {
      env << *rtspClient << "Failed to create a data sink for the \"" << *scs.subsession
          << "\" subsession: " << env.getResultMsg() << "\n";
      break;
    }

    env << *rtspClient << "Created a data sink for the \"" << *scs.subsession << "\" subsession\n";
    scs.subsession->miscPtr = rtspClient; // a hack to let subsession handler functions get the "RTSPClient" from the subsession
    scs.subsession->sink->startPlaying(*(scs.subsession->readSource()),
                                       subsessionAfterPlaying, scs.subsession);
    // Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
    if (scs.subsession->rtcpInstance() != NULL)
    {
      scs.subsession->rtcpInstance()->setByeWithReasonHandler(subsessionByeHandler, scs.subsession);
    }
  } while (0);
  delete[] resultString;

  // Set up the next subsession, if any:
  setupNextSubsession(rtspClient);
}

void continueAfterPLAY(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  Boolean success = False;

  do
  {
    UsageEnvironment &env = rtspClient->envir();                 // alias
    StreamClientState &scs = ((ourRTSPClient *)rtspClient)->scs; // alias

    if (resultCode != 0)
    {
      env << *rtspClient << "Failed to start playing session: " << resultString << "\n";
      break;
    }

    // Set a timer to be handled at the end of the stream's expected duration (if the stream does not already signal its end
    // using a RTCP "BYE").  This is optional.  If, instead, you want to keep the stream active - e.g., so you can later
    // 'seek' back within it and do another RTSP "PLAY" - then you can omit this code.
    // (Alternatively, if you don't want to receive the entire stream, you could set this timer for some shorter value.)
    if (scs.duration > 0)
    {
      unsigned const delaySlop = 2; // number of seconds extra to delay, after the stream's expected duration.  (This is optional.)
      scs.duration += delaySlop;
      unsigned uSecsToDelay = (unsigned)(scs.duration * 1000000);
      scs.streamTimerTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc *)streamTimerHandler, rtspClient);
    }

    env << *rtspClient << "Started playing session";
    if (scs.duration > 0)
    {
      env << " (for up to " << scs.duration << " seconds)";
    }
    env << "...\n";

    success = True;
  } while (0);
  delete[] resultString;

  if (!success)
  {
    // An unrecoverable error occurred with this stream.
    shutdownStream(rtspClient);
  }
}

// Implementation of the other event handlers:

void subsessionAfterPlaying(void *clientData)
{
  MediaSubsession *subsession = (MediaSubsession *)clientData;
  RTSPClient *rtspClient = (RTSPClient *)(subsession->miscPtr);

  // Begin by closing this subsession's stream:
  Medium::close(subsession->sink);
  subsession->sink = NULL;

  // Next, check whether *all* subsessions' streams have now been closed:
  MediaSession &session = subsession->parentSession();
  MediaSubsessionIterator iter(session);
  while ((subsession = iter.next()) != NULL)
  {
    if (subsession->sink != NULL)
      return; // this subsession is still active
  }

  // All subsessions' streams have now been closed, so shutdown the client:
  shutdownStream(rtspClient);
}

void subsessionByeHandler(void *clientData, char const *reason)
{
  MediaSubsession *subsession = (MediaSubsession *)clientData;
  RTSPClient *rtspClient = (RTSPClient *)subsession->miscPtr;
  UsageEnvironment &env = rtspClient->envir(); // alias

  env << *rtspClient << "Received RTCP \"BYE\"";
  if (reason != NULL)
  {
    env << " (reason:\"" << reason << "\")";
    delete[](char *) reason;
  }
  env << " on \"" << *subsession << "\" subsession\n";

  // Now act as if the subsession had closed:
  subsessionAfterPlaying(subsession);
}

void streamTimerHandler(void *clientData)
{
  ourRTSPClient *rtspClient = (ourRTSPClient *)clientData;
  StreamClientState &scs = rtspClient->scs; // alias

  scs.streamTimerTask = NULL;

  // Shut down the stream:
  shutdownStream(rtspClient);
}

void shutdownStream(RTSPClient *rtspClient, int exitCode)
{
  UsageEnvironment &env = rtspClient->envir();                 // alias
  StreamClientState &scs = ((ourRTSPClient *)rtspClient)->scs; // alias

  // First, check whether any subsessions have still to be closed:
  if (scs.session != NULL)
  {
    Boolean someSubsessionsWereActive = False;
    MediaSubsessionIterator iter(*scs.session);
    MediaSubsession *subsession;

    while ((subsession = iter.next()) != NULL)
    {
      if (subsession->sink != NULL)
      {
        Medium::close(subsession->sink);
        subsession->sink = NULL;

        if (subsession->rtcpInstance() != NULL)
        {
          subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
        }

        someSubsessionsWereActive = True;
      }
    }

    if (someSubsessionsWereActive)
    {
      // Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
      // Don't bother handling the response to the "TEARDOWN".
      rtspClient->sendTeardownCommand(*scs.session, NULL);
    }
  }

  env << *rtspClient << "Closing the stream.\n";
  Medium::close(rtspClient);
  // Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.

  if (--rtspClientCount == 0)
  {
    // The final stream has ended, so exit the application now.
    // (Of course, if you're embedding this code into your own application, you might want to comment this out,
    // and replace it with "eventLoopWatchVariable = 1;", so that we leave the LIVE555 event loop, and continue running "main()".)
    exit(exitCode);
  }
}

// Implementation of "ourRTSPClient":

ourRTSPClient *ourRTSPClient::createNew(UsageEnvironment &env, char const *rtspURL,
                                        int verbosityLevel, char const *applicationName, portNumBits tunnelOverHTTPPortNum)
{
  return new ourRTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum);
}

ourRTSPClient::ourRTSPClient(UsageEnvironment &env, char const *rtspURL,
                             int verbosityLevel, char const *applicationName, portNumBits tunnelOverHTTPPortNum)
    : RTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum, -1)
{
}

ourRTSPClient::~ourRTSPClient()
{
}

// Implementation of "StreamClientState":

StreamClientState::StreamClientState()
    : iter(NULL), session(NULL), subsession(NULL), streamTimerTask(NULL), duration(0.0)
{
}

StreamClientState::~StreamClientState()
{
  delete iter;
  if (session != NULL)
  {
    // We also need to delete "session", and unschedule "streamTimerTask" (if set)
    UsageEnvironment &env = session->envir(); // alias

    env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
    Medium::close(session);
  }
}

// Implementation of "DummySink":

// Even though we're not going to be doing anything with the incoming data, we still need to receive it.
// Define the size of the buffer that we'll use:
#define DUMMY_SINK_RECEIVE_BUFFER_SIZE 100000

DummySink *DummySink::createNew(UsageEnvironment &env, MediaSubsession &subsession, char const *streamId)
{
  return new DummySink(env, subsession, streamId);
}

DummySink::DummySink(UsageEnvironment &env, MediaSubsession &subsession, char const *streamId)
    : MediaSink(env),
      fSubsession(subsession)
{
  fStreamId = strDup(streamId);
  fReceiveBuffer = new u_int8_t[DUMMY_SINK_RECEIVE_BUFFER_SIZE + 4];
  char head[4] = {0x00, 0x00, 0x00, 0x01};
  memcpy(fReceiveBuffer, head, 4);
}

DummySink::~DummySink()
{
  delete[] fReceiveBuffer;
  delete[] fStreamId;
}

void DummySink::afterGettingFrame(void *clientData, unsigned frameSize, unsigned numTruncatedBytes,
                                  struct timeval presentationTime, unsigned durationInMicroseconds)
{
  DummySink *sink = (DummySink *)clientData;
  unsigned int SPropRecords = -1;
  SPropRecord *p_record = parseSPropParameterSets(sink->fSubsession.fmtp_spropparametersets(), SPropRecords);
  //sps pps 以数组的形式保存SPropRecord中
  SPropRecord &sps = p_record[0];
  SPropRecord &pps = p_record[1];
  sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

// If you don't want to see debugging output for each received frame, then comment out the following line:
// #define DEBUG_PRINT_EACH_RECEIVED_FRAME 1

void DummySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
                                  struct timeval presentationTime, unsigned /*durationInMicroseconds*/)
{
  // We've just received a frame of data.  (Optionally) print out information about it:
#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
  if (fStreamId != NULL)
    envir() << "Stream \"" << fStreamId << "\"; ";
  envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";
  if (numTruncatedBytes > 0)
    envir() << " (with " << numTruncatedBytes << " bytes truncated)";
  char uSecsStr[6 + 1]; // used to output the 'microseconds' part of the presentation time
  sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
  envir() << ".\tPresentation time: " << (int)presentationTime.tv_sec << "." << uSecsStr;
  if (fSubsession.rtpSource() != NULL && !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP())
  {
    envir() << "!"; // mark the debugging output to indicate that this presentation time is not RTCP-synchronized
  }
#ifdef DEBUG_PRINT_NPT
  envir() << "\tNPT: " << fSubsession.getNormalPlayTime(presentationTime);
#endif
  envir() << "\n";
#endif
  unsigned char const start_code[4] = {0x00, 0x00, 0x00, 0x01};
  static unsigned char *tmp = NULL;

  if (!fHaveWrittenFirstFrame)
  {
    unsigned numSPropRecords;
    SPropRecord *sPropRecords = parseSPropParameterSets(fSubsession.fmtp_spropparametersets(), numSPropRecords);
    envir() << "numSPropRecords = " << numSPropRecords << "\n";
    unsigned int totalsize = 0;
    for (unsigned i = 0; i < numSPropRecords; ++i)
    {
      totalsize = totalsize + 4 + sPropRecords[i].sPropLength;
    }
    envir() << "totalsize = " << totalsize << "\n";
    tmp = (unsigned char *)realloc(tmp, totalsize);
    c->extradata_size = totalsize;
    c->extradata = tmp;
    for (unsigned i = 0; i < numSPropRecords; ++i)
    {
      memcpy(tmp, start_code, 4);
      memcpy(tmp + 4, sPropRecords[i].sPropBytes, sPropRecords[i].sPropLength);
      tmp = tmp + 4 + sPropRecords[i].sPropLength;
      printf("sPropRecords[%d].sPropLength = %d\n", i, sPropRecords[i].sPropLength);
    }
    delete[] sPropRecords;
    fHaveWrittenFirstFrame = True; // for next time
  }
  for (size_t i = 0; i < 22; i++)
  {
    printf("%0.2X ", *(c->extradata + i));
  }
  printf("\n");
  // if(get_char() == 27) //SDL运行的时候不能在这里检测键盘
  // {
  //   if(SDLInit)
  //   {
  //     sdl_stop();
  //   }
  //   printf("exit ....");
  //   exit(0);
  // }
  decoderyuv(fReceiveBuffer, frameSize);

  // Then continue, to request the next frame of data:
  continuePlaying();
}

Boolean DummySink::continuePlaying()
{
  if (fSource == NULL)
    return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame(fReceiveBuffer + 4, DUMMY_SINK_RECEIVE_BUFFER_SIZE,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}

void decode_init(void)
{
  avcodec_register_all();
  codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (NULL == codec)
  {
    fprintf(stderr, "Codec not found\n");
    exit(1);
  }

  c = avcodec_alloc_context3(codec);
  if (NULL == c)
  {
    fprintf(stderr, "Could not allocate video codec context\n");
    exit(1);
  }

  avParserContext = av_parser_init(AV_CODEC_ID_H264);
  if (NULL == avParserContext)
  {
    fprintf(stderr, "Could not init avParserContext\n");
    exit(1);
  }

  if (avcodec_open2(c, codec, NULL) < 0)
  {
    fprintf(stderr, "Could not open codec\n");
    exit(1);
  }

  frame = av_frame_alloc();
  if (NULL == frame)
  {
    fprintf(stderr, "Could not allocate video frame\n");
    exit(1);
  }
  pFrameYUV = av_frame_alloc();
}

int decoderyuv(unsigned char *inbuf, int read_size)
{
  int got_frame;
  unsigned char *buf = (unsigned char *)malloc((read_size + c->extradata_size) * 2);
  printf("read_size = %d, extradata_size = %d , total size = %d\n", read_size, c->extradata_size, read_size + c->extradata_size);
  memcpy(buf, c->extradata, c->extradata_size);
  memcpy(buf + c->extradata_size, inbuf, read_size);
  AVPacket avpkt = {0};
  av_init_packet(&avpkt);
  avpkt.data = buf;
  avpkt.size = read_size + c->extradata_size;
  int decode_len = avcodec_decode_video2(c, frame, &got_frame, &avpkt);
  std::cout << "decode_len = " << decode_len << std::endl;
  // exit(1);
  if (decode_len < 0)
    fprintf(stderr, "Error while decoding frame \n");
  if (got_frame)
  {
    std::cout << "width = " << frame->width << ", height = " << frame->height << std::endl;
    if (!SDLInit)
    {
      sdl_init(frame->width, frame->height);
      // sdl_init(100, 100);
      SDLInit = true;
    }
    printf("c->width = %d, c->height = %d, c->pix_fmt = %d\n", c->width, c->height, c->pix_fmt);
    enum AVPixelFormat FMT = AV_PIX_FMT_NV12;
    uint8_t *yuv = (uint8_t *)av_malloc(avpicture_get_size(FMT, frame->width, frame->height) * sizeof(uint8_t));
    avpicture_fill((AVPicture *)pFrameYUV, yuv, FMT, frame->width, frame->height);
    img_convert_ctx = sws_getContext(c->width, c->height, c->pix_fmt, c->width, c->height, FMT, 2, NULL, NULL, NULL);
    sws_scale(img_convert_ctx, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, pFrameYUV->data, pFrameYUV->linesize);
    SDL_UpdateTexture(sdlTexture, NULL, yuv, c->width);

    sdlRect.x = 0;
    sdlRect.y = 0;
    sdlRect.w = c->width;
    sdlRect.h = c->height;

    SDL_RenderClear(sdlRenderer);
    SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
    SDL_RenderPresent(sdlRenderer);

    av_free(yuv);
    // SDL_Delay(33);
  }
  free(buf);
}

void sdl_init(unsigned int width, unsigned int height)
{
  if (SDL_Init(SDL_INIT_VIDEO))
  {
    printf("Could not initialize SDL - %s\n", SDL_GetError());
    exit(1);
  }
  sdlWindow = SDL_CreateWindow("Simplest Video Play SDL2", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                               width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (sdlWindow == 0)
  {
    printf("SDL: could not create SDL_Window - exiting:%s\n", SDL_GetError());
    exit(1);
  }

  sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED);
  if (sdlRenderer == NULL)
  {
    printf("SDL: could not create SDL_Renderer - exiting:%s\n", SDL_GetError());
    exit(1);
  }
  sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, width, height);
  if (sdlTexture == NULL)
  {
    printf("SDL: could not create SDL_Texture - exiting:%s\n", SDL_GetError());
    exit(1);
  }
}

void sdl_stop(void)
{
  SDL_DestroyTexture(sdlTexture);
  SDL_DestroyRenderer(sdlRenderer);
  SDL_DestroyWindow(sdlWindow);
  SDL_Quit();
}

int get_char()
{
    fd_set rfds;
    struct timeval tv;
    int ch = 0;

    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 1; //设置等待超时时间

    //检测键盘是否有输入
    if (select(1, &rfds, NULL, NULL, &tv) > 0)
    {
        ch = getchar(); 
    }

    return ch;
}