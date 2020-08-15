#include <stdexcept>
#include <stdio.h>
#include <iostream>
#include <string.h>
#include <future>
#include <time.h>
#include <vector>
#include <dirent.h>
#include "WavFile.h"
#include <soundtouch/SoundTouch.h>
#include <alsa/asoundlib.h>
#include <csignal>

using namespace soundtouch;
using namespace std;

// Processing chunk size (size chosen to be divisible by 2, 4, 6, 8, 10, 12, 14, 16 channels ...)
#define BUFF_SIZE  4096*2
//#define PCM_DEVICE "plughw:1,0"
#define PCM_DEVICE "plughw:1,0"
#define RATE 44100
#define CHANNELS 2


struct ctx {
  // pcm_in
  snd_pcm_t *pcm_handle_in;
  // pcm_out
  snd_pcm_t *pcm_handle_out;
  // output file
  WavOutFile *outFile;
	// file name
  string fname;
};
struct ctx ctx;

static const char _helloText[] = 
"\n"
"   Chopage v%s -  Copyright (c) Dichtomas Monk\n"
"=========================================================\n";


static snd_pcm_t* initPCM(snd_pcm_stream_t stream){

  // pcm init
  unsigned int pcm;
  snd_pcm_hw_params_t *params;
  snd_pcm_t *pcm_handle;

	if (pcm = snd_pcm_open(&pcm_handle, PCM_DEVICE,
				stream, 0) < 0)
		printf("ERROR: Can't open \"%s\" PCM. %s\n",
				stream, snd_strerror(pcm));

  // aloc params with default values
  snd_pcm_hw_params_alloca(&params);
  snd_pcm_hw_params_any(pcm_handle, params);

  // override defaults
  if (pcm = snd_pcm_hw_params_set_format(pcm_handle, params,
        SND_PCM_FORMAT_FLOAT_LE) < 0)
    printf("ERROR: Can't set format. %s\n", snd_strerror(pcm));

  if (pcm =  snd_pcm_hw_params_set_access(pcm_handle, params,
        SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
    printf("ERROR: Can't set access. %s\n", snd_strerror(pcm));

  if (pcm = snd_pcm_hw_params_set_channels(pcm_handle, params, CHANNELS) < 0) 
    printf("ERROR: Can't set channels number. %s\n", snd_strerror(pcm));

  unsigned int rate = RATE;
  if (pcm = snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, 0) < 0) 
    printf("ERROR: Can't set rate. %s\n", snd_strerror(pcm));


  if (pcm = snd_pcm_hw_params_set_buffer_size(pcm_handle, params, BUFF_SIZE/2) < 0) 
    printf("ERROR: Can't set buffersize. %s\n", snd_strerror(pcm));

  unsigned int period = 2; 
  if (pcm = snd_pcm_hw_params_set_period_time_near(pcm_handle, params, &period, 0) < 0) 
    printf("ERROR: Can't set period time. %s\n", snd_strerror(pcm));

  /* Write parameters */
  if (pcm = snd_pcm_hw_params(pcm_handle, params) < 0)
    printf("ERROR: Can't set harware parameters. %s\n", snd_strerror(pcm));

  snd_pcm_uframes_t   	buffer_size; 
  snd_pcm_uframes_t   	period_size;
  snd_pcm_get_params(pcm_handle, &buffer_size, &period_size);
  printf("PCM OPENED WITH: %ld buffer, %ld period\n", buffer_size, period_size);

	if ((pcm = snd_pcm_prepare (pcm_handle)) < 0) {
	 	fprintf (stderr, "cannot prepare audio interface for use (%s)\n",
	 					 snd_strerror (pcm));
	 	exit (1);
	}

  return pcm_handle;
}


// Read and write an audio sample
void sampleAudio(struct ctx *ctx){

	SAMPLETYPE *buff = new SAMPLETYPE[BUFF_SIZE];
  int nSamples = BUFF_SIZE/CHANNELS;
  int err;
 	if ((err = snd_pcm_readi(ctx->pcm_handle_in, buff, nSamples)) != nSamples) {
    printf("read err %s\n", snd_strerror(err));
    snd_pcm_prepare(ctx->pcm_handle_in);
  }

 if ((err = snd_pcm_writei(ctx->pcm_handle_out, buff, nSamples)) < 0){
   printf("write err %s\n", snd_strerror(err));
 	 snd_pcm_prepare(ctx->pcm_handle_out);
 } else {
   ctx->outFile->write(buff, BUFF_SIZE);
 }
}




void openSampleFile(struct ctx *ctx){
	time_t t = time(0);   // get time now
 	struct tm * now = localtime( & t );

	char buffer [80];
	strftime(buffer,80,"/home/pi/chopogy/samples/%Y-%m-%d-%M-%S.wav", now);
  ctx->fname = string(buffer);
  FILE *file = fopen(buffer, "wb");
  ctx->outFile = new WavOutFile(file, RATE, 32, CHANNELS);

}


void signalTermHandler( int signum ) {
   printf("Interrupt signal %d\n" ,signum);

   // cleanup and close up stuff here  
   // terminate program  
   delete ctx.outFile;
   std::cout<<"Save: "<<ctx.fname <<std::endl;
   exit(signum);  
}

void signalIntHandler( int signum ) {
   printf("Int signal %d\n" ,signum);

   // cleanup and close up stuff here  
   // terminate program  
   delete ctx.outFile;

    // delete tmpfile 
   std::cout<<"Remove: "<<ctx.fname <<std::endl;
	 unlink(ctx.fname.c_str()); 
	 remove(ctx.fname.c_str()); 

   exit(signum);  
}

int main(const int nParams, const char * const paramStr[])
{
  signal(SIGTERM, signalTermHandler);
  signal(SIGINT, signalIntHandler);

  try 
  {
		// Open in and out
 	 ctx.pcm_handle_out = initPCM(SND_PCM_STREAM_PLAYBACK);
   if (ctx.pcm_handle_out == 0)
     return -1;

		ctx.pcm_handle_in = initPCM(SND_PCM_STREAM_CAPTURE);
    if (ctx.pcm_handle_in == 0)
      return -1;

    // open output file
    openSampleFile(&ctx);

    // Run controller 
    while (1) {
      sampleAudio(&ctx);
    }

    fprintf(stderr, "Done!\n");
  } 

  catch (const runtime_error &e) 
  {
    // An exception occurred during processing, display an error message
    fprintf(stderr, "%s\n", e.what());
    return -1;
  }

  return 0;
}
