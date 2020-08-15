#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include <future>
#include <time.h>
#include <vector>
#include <dirent.h>
#include "RunParameters.h"
#include "WavFile.h"
#include <soundtouch/SoundTouch.h>
#include <soundtouch/BPMDetect.h>
#include <alsa/asoundlib.h>
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>
#include <queue>

using namespace soundtouch;
using namespace std;

#define BUFF_SIZE 2048
#define PCM_DEVICE "default"
#define RATE 44100
#define TEMPO_CTL 0x12 
#define PITCH_CTL 0x13
//#define RATE_CTL 0x56
#define RATE_CTL 0x72
#define SLICE_START_CTL 0x4a
#define SLICE_END_CTL 0x47
#define MODE_CTL 0x50
#define CHAN_CTL 0x52
#define SCAN_CTL 0x7
#define SUPER_LOW_KEY 24
#define CHANNELS 2
#define SOUNDTOUCH_INTEGER_SAMPLES 1
#define SET_STREAM_TO_BIN_MODE(f) {}
#define MAX_PCM_HANDLES 8
#define MAX_CHANNELS 16 
#define MAX_SLICES 88 
#define MAX_TRACKS 32 


struct slice {
	long unsigned int start;
	long unsigned int start_offset;
	long unsigned int end;
	long unsigned int end_offset;
  int channel;
  unsigned int note;
};

struct sample {
  string fname;
  unsigned int channel;
  int bits;
	// lowest key in range
	int low_key;
	vector<SAMPLETYPE*> *buffers;
  WavInFile *file;
	slice slices[MAX_SLICES];
	slice *selectedSlice;
  int bpm;
};

// wrapper for pcm type 
struct pcm_handle {
	snd_pcm_t *pcm;
};

enum chp_program{CHP_BROWSE = 1,
	CHP_ASSIGN = 2,
	CHP_EDIT = 3,
	CHP_LOAD_SLC = 4,
	CHP_SAVE_SLC = 5,
  CHP_SET_FX = 6,
  CHP_SET_CH = 7,
	CHP_SAVE_PACK = 8,
	CHP_LOAD_PACK,
  CHP_DELETE,
  CHP_REC_START,
	CHP_REC_STOP,
	CHP_LOAD_SAMP};

struct track {
  WavOutFile *file;
	int num;
	
};

struct ctx {

  // program
  chp_program prog;

  // pcm_outputs
  pcm_handle pcm_handles[MAX_PCM_HANDLES];
	int num_pcm_handles;

  // cursor of pcms
	unsigned int pcm_cursor;

  // sample browser
  vector<sample *> snippets;

  // active samples 
  sample samples[MAX_CHANNELS];

  // manual override for midi chan
  int midi_chan_override;

  // selected sample to edit
	sample *selectedSnippet;

  // modulation 
  SoundTouch soundTouch;

  //fx_chan
  int fx_chan;

  // midi 
  snd_seq_t *seq_handle;

	// mixer
	track tracks[MAX_TRACKS];

	// active recording track
	int active_rec_track;

};

// thread id running
atomic_uchar tid[MAX_CHANNELS];

static const char _helloText[] = 
"\n"
"   Chopage v%s -  Copyright (c) Dichtomas Monk\n"
"=========================================================\n";

// Open all files and store frames in buffer
int openFiles(WavInFile **inFile, struct ctx *ctx)
{

  // open snippets...
  DIR *dir;
  struct dirent *ent;
  string path = "/home/pi/chopogy/samples/";
  dir = opendir(path.c_str());
  if (dir == NULL) {
    fprintf(stderr, "Unable to open dir %s\n", path);
    return -1;
  }
  do {
    ent = readdir(dir);
    if (ent != NULL){
      char *fname = ent->d_name;
      if (strstr(fname, ".wav") != NULL){
        string p(path);
        p.append(fname);
        WavInFile *wf = new WavInFile(p.c_str());
        
        // init sample 
        struct sample *s = new sample();
        s->file = wf;
        s->buffers = new vector<SAMPLETYPE*>(0);
				s->low_key = 0;
        s->fname = string(ent->d_name);
				s->fname.replace(s->fname.size()-4,4,"");

        // read preview frames to memory
        // TODO: make it a slice
        int previewCount = 100;
        while ((wf->eof() == 0) && (previewCount-- > 0)){
          int num;
          SAMPLETYPE *buff = new SAMPLETYPE[BUFF_SIZE];
          num = wf->read(buff, BUFF_SIZE);
          s->buffers->push_back(buff);

        }

        // printf("Read %s\n", p.c_str());

        // add sample to context
        ctx->snippets.push_back(s);
      }
    }
  } while (ent != NULL);

  closedir (dir);
  return 0;
}

void initTrack( ctx *ctx, int songId, int channel){
	string sampleName =  ctx->samples[channel].fname;
	string filepath = "/home/pi/chopogy/songs/"+to_string(songId)+"/track-";
	filepath.append(to_string(channel)+"_"+sampleName+".wav");
	// channel is used as trackNum
	ctx->tracks[channel].num = channel;
	ctx->tracks[channel].file = new WavOutFile(filepath.c_str(), RATE, 32, CHANNELS);
	cout << "Opened: "<<filepath<<" for writing"<<endl;
}

string packName(int pack, int channel){
	return "/home/pi/chopogy/packs/"+to_string(pack)+"/"+to_string(channel+1)+".pack.yaml";
}

string sampleToYamlFilename(string sampleName, int channel){
	string filepath = "/home/pi/chopogy/slices/"+to_string(channel+1)+"/";
	string name(sampleName);
	name.append(".yaml");
	filepath.append(name);
	return filepath;
}
// re-populate the sample buffers
void loadSelectedSnippet(ctx *ctx, int midi_chan){

  // get selected snippet
  sample *snip = NULL;
	if (ctx->selectedSnippet != NULL){
		snip = ctx->selectedSnippet;
	} else if (ctx->snippets.size() > 0){
		snip = ctx->snippets.at(0);
	} else {
		fprintf (stderr, "Could not select a sample to load\n");
		return;
	}


  // unload sample currently loaded
  // for selected channel 
  sample *chan_sample = &ctx->samples[midi_chan];
  if ( chan_sample->buffers != NULL){
		delete chan_sample->buffers;
    // clear slices
    for (int i = 0; i < MAX_SLICES; i++){
      chan_sample->slices[i].note = 0;
      chan_sample->slices[i].start = 0;
      chan_sample->slices[i].start_offset = 0;
      chan_sample->slices[i].end = 0;
      chan_sample->slices[i].end_offset = 0;
      chan_sample->slices[i].channel = 0;
    }
  } 
 	chan_sample->buffers = new vector<SAMPLETYPE*>(0);


  // init bpm analyzer
  int nChannels = (int)snip->file->getNumChannels();
  BPMDetect bpm(nChannels, snip->file->getSampleRate());
 
 	// read from snippet file pointer to channel sample
	snip->file->rewind();
  while (snip->file->eof() == 0){
    int num;
    SAMPLETYPE *buff = new SAMPLETYPE[BUFF_SIZE];
    num = snip->file->read(buff, BUFF_SIZE);
    chan_sample->buffers->push_back(buff);

    // Enter the new samples to the bpm analyzer class
    bpm.inputSamples(buff, num / nChannels);
  }

  chan_sample->bpm = bpm.getBpm();

	// attempt to restore slices if in LOAD prog
	if (ctx->prog==CHP_LOAD_SLC){
		string fname = sampleToYamlFilename(snip->fname, midi_chan);
		ifstream fin(fname.c_str());
		if (fin.good()){
			YAML::Node config = YAML::Load(fin);
			YAML::Node settings = config["settings"];
			YAML::Node slices = config["slices"];
			for (std::size_t i=0;i<slices.size();i++) {
					YAML::Node n = slices[i];
					int note = n["note"].as<int>();	
					chan_sample->slices[note].start = n["start"].as<int>();
					chan_sample->slices[note].start_offset = n["start_offset"].as<int>();
					chan_sample->slices[note].end = n["end"].as<int>();
					chan_sample->slices[note].end_offset = n["end_offset"].as<int>();
			}
			cout << "Restored: "<<fname<<endl;
		}
  }

	chan_sample->fname = snip->fname;

	chan_sample->channel = midi_chan;
	printf("Loaded on Ch:%d (%d bpm)\n", midi_chan+1, chan_sample->bpm);
}

void loadSamplePack(struct ctx *ctx, int pack, int channel){

	// restore packs if exists
	string fname = packName(pack, channel);
  ifstream fin(fname.c_str());
  if (fin.good()){
		YAML::Node config = YAML::Load(fin);
		YAML::Node samples = config["samples"];
		for (std::size_t i=0;i<samples.size();i++) {
      YAML::Node n = samples[i];
      int channel = n["channel"].as<int>();
      string slices = n["slices"].as<string>();
			// load sample from snippet
			for (int i = 0; i < ctx->snippets.size(); i++){
				sample *snip = ctx->snippets.at(i);
				// compare name of pack slices to known snippets
      	if (slices == snip->fname){
					ctx->selectedSnippet = snip;
					loadSelectedSnippet(ctx, channel);
				}
			}
		}
	}
}

snd_pcm_t* initPCM(snd_pcm_stream_t stream, const char *device){

  // pcm init
  unsigned int pcm;
  snd_pcm_hw_params_t *params;
  snd_pcm_t *pcm_handle;

	if (pcm = snd_pcm_open(&pcm_handle, device,
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


	snd_pcm_uframes_t val = 0;
	snd_pcm_hw_params_get_buffer_size_max(params, &val); 
  int ret = snd_pcm_hw_params_set_buffer_size(pcm_handle, params, val/2);
  if (ret < 0)
    printf("ERROR: Can't set buffersize. %d, %s\n", ret,  snd_strerror(ret));


  int dir = 0;
	snd_pcm_hw_params_get_period_size_min(params, &val, &dir); 
  ret = snd_pcm_hw_params_set_period_size(pcm_handle, params, val, dir);
  if (ret < 0)
    printf("ERROR: Can't set period size. %d, %s\n", ret,  snd_strerror(ret));


	unsigned int period = 0;
	snd_pcm_hw_params_get_period_time_min(params, &period, &dir); 
  if (pcm = snd_pcm_hw_params_set_period_time(pcm_handle, params, period, dir) < 0) 
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

// Sets the 'SoundTouch' object up according to input file sound format & 
// command line parameters
void setup(SoundTouch *pSoundTouch)
{
  int sampleRate;
  int channels;

  pSoundTouch->setSampleRate(RATE);
  pSoundTouch->setChannels(CHANNELS);

  pSoundTouch->setTempoChange(0);
  pSoundTouch->setPitchSemiTones(0);
  pSoundTouch->setRateChange(0);

  //pSoundTouch->setSetting(SETTING_USE_QUICKSEEK, params->quick);
  //pSoundTouch->setSetting(SETTING_USE_AA_FILTER, !(params->noAntiAlias));

  fflush(stderr);
}

void openMidi(struct ctx *ctx)
{
  int err;
  err = snd_seq_open(&ctx->seq_handle, "default", SND_SEQ_OPEN_INPUT, 0);
  if (err < 0)
    fprintf(stderr, "Failed to open midi sequencer\n");

  snd_seq_set_client_name(ctx->seq_handle, "Chopogy");
  err = snd_seq_create_simple_port(ctx->seq_handle, "Chopogy Input",
      SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_APPLICATION);
  if (err < 0)
    fprintf(stderr, "Failed to create midi port\n");

  // connect to midi through
  if ((err = snd_seq_connect_from(ctx->seq_handle, 0, 14, 0)) < 0)
    fprintf(stderr, "Failed to connect midi through port\n");
}

snd_pcm_t* pcm_handle(ctx *ctx) {
	ctx->pcm_cursor = (ctx->pcm_cursor+1) % ctx->num_pcm_handles;
	return ctx->pcm_handles[ctx->pcm_cursor].pcm;
}

snd_pcm_t* pcm_handle_i(ctx *ctx, int index) {
	return ctx->pcm_handles[index].pcm;
}

// pass in the interval 
void play_sample(ctx *ctx, sample *s, slice *slc, unsigned char thread_id)
{

  int err, nSamples;
	long unsigned int start, end;

  unsigned int nBuffers = s->buffers->size();
  SAMPLETYPE *buff = new SAMPLETYPE[BUFF_SIZE];

  int slc_chan = 0;
	if (slc == NULL){
		start = 0;
		end = nBuffers;
	} else {
		start = slc->start + slc->start_offset;
		if (ctx->prog == CHP_ASSIGN){
			// play to end in edit mode because this can be changed
			end = nBuffers;
		} else {
			end = slc->end + slc->end_offset;
		}
    slc_chan = slc->channel;
	}

	snd_pcm_t *pcm = pcm_handle(ctx);
  snd_pcm_prepare(pcm);

  for (long unsigned int i=start; i<end; i++){
    unsigned char diff = tid[slc_chan].load() - thread_id;
    if (diff != 0) {
      // stopping
  		// snd_pcm_drop(pcm);
  		// snd_pcm_prepare(pcm);
			// update slice end
			if ((slc != NULL) && (ctx->prog == CHP_ASSIGN)) {
        // end earlier than what we heard
        // for help with better editing
				slc->end = i-10;
			}
      return;  
    }

    nSamples = BUFF_SIZE/CHANNELS;
    memcpy(buff, s->buffers->at(i), BUFF_SIZE*4);

    // Feed the samples into SoundTouch processor
	  if (slc_chan == ctx->fx_chan){
	    ctx->soundTouch.putSamples(buff, nSamples);
	    nSamples = ctx->soundTouch.receiveSamples(buff, nSamples);
	  }

    // output
  if ((err = snd_pcm_writei(pcm, buff, nSamples)) < 0){
    printf("write err %s (%d) \n", snd_strerror(err), err);
  	snd_pcm_prepare(pcm);
  } else {
		if (err != nSamples){
   	 snd_pcm_prepare(pcm);
	  }
	}
  }
 	snd_pcm_prepare(pcm);
}

// Play just a slice of sample
void play_slice(ctx *ctx, sample *s, int note, int channel, unsigned char thread_id)
{

  int pcm, nSamples;
  unsigned int nBuffers = s->buffers->size();
  SAMPLETYPE *buff = new SAMPLETYPE[BUFF_SIZE];
	
  // get slice associated with note
	slice *slc = &ctx->samples[channel].slices[note];
	slc->note = note;
	// if slice interval is not set then start at
	// end of closest note with lower interval
	long unsigned int startPos = slc->start;
	if ((startPos == 0) && (note > s->low_key)) {
		for (int i = note; i > 0; i--){
			startPos = ctx->samples[channel].slices[i].end;
			if (startPos > 0){
				slc->start = startPos;
				break;
			}
		}
	}

	// setting as low key since no previous key has interval
	if ((startPos == 0) && (s->low_key == 0)) {
		s->low_key = note;
	}
  slc->channel = channel;
	s->selectedSlice = slc;
	play_sample(ctx, s, slc, thread_id);
	s->slices[note] = *slc;
}



// dump loaded for samples and slices
void dumpSamplePack(struct ctx *ctx, int pack, int channel){

  YAML::Emitter out;

	// get loaded samples
	out << YAML::BeginMap;
	out << YAML::Key << "samples";
	out << YAML::Value << YAML::BeginSeq;
	for (int channel = 0; channel < MAX_CHANNELS; channel++){
		sample *s = &ctx->samples[channel];
		string fname = sampleToYamlFilename(s->fname, channel);
		ifstream fin(fname.c_str());
		if (fin.good()){
			out << YAML::BeginMap;
			out << YAML::Key << "channel";
			out << YAML::Value << channel;
			out << YAML::Key << "slices";
			out << YAML::Value << s->fname;
			out << YAML::EndMap;
		}
	}
	out << YAML::EndSeq;
  out << YAML::EndMap;
  printf("---\n%s\n", out.c_str());

	// dump to file 
 	ofstream pack_file;
	string fname = packName(pack, channel); 
	cout << "Pack: "<<fname << endl;
  pack_file.open (fname.c_str());
  pack_file << out.c_str();
  pack_file.close();
}

// dump slices for sample
void dumpSampleSlices(struct ctx *ctx, int channel){

  YAML::Emitter out;
  struct slice slc;
  sample *s = &ctx->samples[channel];

	// get settings
  map <std::string, int> settings;
	settings["pitch"] = 0;
	settings["rate"] = 0;
	settings["tempo"] = 0;

  // get slices
  vector <map <std::string, int>> slices;
  for (int i = 0; i < MAX_SLICES; i++){
    slc = s->slices[i];
    if (slc.end > 0){
      slices.push_back({
      	{"note",slc.note},
      	{"start",slc.start},
      	{"start_offset",slc.start_offset},
      	{"end",slc.end},
      	{"end_offset",slc.end_offset},
      });
    }
  }

	// convert to yaml
	out << YAML::BeginMap;
	out << YAML::Key << "settings";
	out << YAML::Value << settings;
	out << YAML::Key << "slices";
	out << YAML::Value <<  YAML::Flow << slices;
	out << YAML::EndMap;
  printf("---\n%s\n", out.c_str());

	// dump to file 
 	ofstream slice_file;
	string fname = sampleToYamlFilename(s->fname, channel);
  slice_file.open (fname.c_str());
  slice_file << out.c_str();
  slice_file.close();
}

snd_seq_event_t *readMidi(struct ctx *ctx)
{
  snd_seq_event_t *ev = NULL;
  int err;
	if ((err = snd_seq_event_input(ctx->seq_handle, &ev)) < 0){
		if (err != -EAGAIN){
    	printf("midi event err: %s\n", snd_strerror(err));
		}
		return NULL;
  }

  // save last_prog for continuous commands
	chp_program last_prog = ctx->prog;
  if ((ev->type == SND_SEQ_EVENT_NOTEON)||(ev->type == SND_SEQ_EVENT_NOTEOFF)) {
    const char *type = (ev->type == SND_SEQ_EVENT_NOTEON) ? "on " : "off";
	  int midi_chan = ev->data.note.channel;
		// Midi chan override can be used for controllers that only
    // operator on single channel
//	if((midi_chan != 1) && (ctx->midi_chan_override != -1)){
//		midi_chan = ctx->midi_chan_override;
//  	ev->data.note.channel = midi_chan;
//	}
    if ((ev->type == SND_SEQ_EVENT_NOTEON) && (ev->data.note.velocity > 0)){
      /* printf("[%d] Note %s: %2x vel(%2x)\n", midi_chan+1, type,
          ev->data.note.note,
          ev->data.note.velocity);*/

			if (last_prog == CHP_SET_CH) {
 				// changing channel
				 ctx->midi_chan_override = ev->data.note.note - 0x24;
				 printf("Channel is:  %2x \n", ctx->midi_chan_override);
         return NULL;
			}
			// update thread id
			// this will also stop other running samples
      tid[midi_chan].store(tid[midi_chan].load()+1);
      ctx->soundTouch.clear();

			// play sample based on program
			if (ctx->prog == CHP_BROWSE){

			  // no matter how many notes we have
				// sample will be spread across them all
				int index = ev->data.note.note % ctx->snippets.size();
  			// update selected sample 
  			ctx->selectedSnippet = ctx->snippets.at(index);
				thread t1(play_sample, ctx, ctx->selectedSnippet, nullptr, tid[midi_chan].load());
				t1.detach();
			} else {
  			// update selected for channel 
  			sample *s = &ctx->samples[midi_chan];
        // do not try and unloaded sample
        if (s->buffers == NULL){
          printf("No slices on channel %d\n", midi_chan);
          return NULL;
        }

				// play slice
				int note = ev->data.note.note;
				thread t1(play_slice, ctx, s, note,ev->data.note.channel, tid[midi_chan].load());
				t1.detach();
			}
    } else {
      // stop sample on key up when in edit/browse mode
			if ((ctx->prog == CHP_ASSIGN)||(ctx->prog == CHP_BROWSE)){
      	tid[midi_chan].store(tid[midi_chan].load()+1);
			}
    }
  } else if (ev->type == SND_SEQ_EVENT_PGMCHANGE) {
    printf("Program Change:  %2x \n", ev->data.control.value);
	  ctx->prog = chp_program(ev->data.control.value);
    int midi_chan = ev->data.note.channel;
    if (ctx->midi_chan_override != -1){
    	midi_chan = ctx->midi_chan_override;
  	}
    // apply fx to whatever chan we are on
    ctx->fx_chan = midi_chan;
		if (ctx->prog == CHP_SAVE_SLC){
		 	// dump slices
      dumpSampleSlices(ctx, midi_chan);
		} else if (ctx->prog == CHP_ASSIGN){
			loadSelectedSnippet(ctx, midi_chan);
		} else if (ctx->prog == CHP_LOAD_SLC){
		 	// load slices
			loadSelectedSnippet(ctx, midi_chan);
		} else if (ctx->prog == CHP_LOAD_SAMP){
		 	// load whole sample 
			loadSelectedSnippet(ctx, midi_chan);
		} else if (ctx->prog == CHP_SET_FX){
      ctx->fx_chan = midi_chan;
		} else if (ctx->prog == CHP_DELETE){
			// delete selected file
  		string path = "/home/pi/chopogy/samples/";
			path.append(ctx->selectedSnippet->fname);
      path.append(".wav");
      cout<<"Remove: "<<path<<endl;
			remove(path.c_str());
		}  else if (ctx->prog == CHP_REC_STOP){
			delete ctx->tracks[ctx->active_rec_track].file;
			ctx->active_rec_track = -1;
    } else if (last_prog == CHP_REC_START){
			// create a track based on prog value 
			ctx->active_rec_track = midi_chan;
			initTrack(ctx, ev->data.control.value, midi_chan);	
		} else if (last_prog == CHP_SAVE_PACK){
			// dump pack
      dumpSamplePack(ctx, ev->data.control.value, midi_chan);
		} else if (last_prog == CHP_LOAD_PACK){
			// load pack
      loadSamplePack(ctx, ev->data.control.value, midi_chan);
		}

  } else if(ev->type == SND_SEQ_EVENT_CONTROLLER) {
    printf("Control:  %2x val(%2x)\n", ev->data.control.param,
        ev->data.control.value);


		// override midi chan
		if (ev->data.control.param == CHAN_CTL){
			ctx->midi_chan_override = ev->data.control.value;
		}

		// snippet scanner
		if (ev->data.control.param == SCAN_CTL){
	  		int midi_chan = ev->data.note.channel;
			  // no matter how many notes we have
				// sample will be spread across them all
				int index = ev->data.control.value % ctx->snippets.size();
  			// update selected sample 
  			ctx->selectedSnippet = ctx->snippets.at(index);
        cout<< "Play: "<< ctx->selectedSnippet->fname << endl;
				thread t1(play_sample, ctx, ctx->selectedSnippet, nullptr, tid[midi_chan].load());
				t1.detach();

    }

		// start slice editor
		if (ev->data.control.param == SLICE_START_CTL){
      sample *chan_sample = &ctx->samples[ev->data.control.channel];
			if (chan_sample != NULL){
				if (chan_sample->selectedSlice != NULL){
					chan_sample->selectedSlice->start_offset = ev->data.control.value - 64;
				}
			}
		}
		// end slice editor
		if (ev->data.control.param == SLICE_END_CTL){
      sample *chan_sample = &ctx->samples[ev->data.control.channel];
			if (chan_sample != NULL){
				if (chan_sample->selectedSlice != NULL){
					chan_sample->selectedSlice->end_offset = ev->data.control.value - 64;
				}
			}
		}

		// adjust fx

    // change tempo at same pitch
    if(ev->data.control.param == TEMPO_CTL){
      int tempo = ev->data.control.value - 64;
      printf("Tempo: %d\n", tempo);
      ctx->soundTouch.setTempoChange(tempo);
    }

    // change pitch	at same tempo 
    if(ev->data.control.param == PITCH_CTL){
      int pitch = ev->data.control.value/4 - 16;
      printf("Pitch: %d\n", pitch);
      ctx->soundTouch.setPitchSemiTones(pitch);
    }

		// change both tempo and pitch
    if(ev->data.control.param == RATE_CTL){
      int rate = ev->data.control.value - 64;
      ctx->soundTouch.setRateChange(rate);
      printf("Rate: %d\n", rate);
    }



  } else if (ev->type == SND_SEQ_EVENT_PORT_SUBSCRIBED){
    printf("Connected to midi controller\n");
  } else if (ev->type == SND_SEQ_EVENT_SENSING){
    // ignore
  } else if (ev->type == SND_SEQ_EVENT_PITCHBEND) {
    // printf("Pitch Bend %d\n", ev->data.control.value);
  } else if (ev != NULL) {
    // printf("[%d] Unknown:  Unhandled Event Received %d\n", ev->time.tick, ev->type);
	} else {
		printf("Unkown error");
  }
  return ev;
}




// Detect BPM rate of inFile and adjust tempo setting accordingly if necessary
void detectBPM(WavInFile *inFile, RunParameters *params)
{
  float bpmValue;
  int nChannels;
  BPMDetect bpm(inFile->getNumChannels(), inFile->getSampleRate());
  SAMPLETYPE sampleBuffer[BUFF_SIZE];

  // detect bpm rate
  fprintf(stderr, "Detecting BPM rate...");
  fflush(stderr);

  nChannels = (int)inFile->getNumChannels();

  // Process the 'inFile' in small blocks, repeat until whole file has 
  // been processed
  while (inFile->eof() == 0)
  {
    int num, samples;

    // Read sample data from input file
    num = inFile->read(sampleBuffer, BUFF_SIZE);

    // Enter the new samples to the bpm analyzer class
    samples = num / nChannels;
    bpm.inputSamples(sampleBuffer, samples);
  }

  // Now the whole song data has been analyzed. Read the resulting bpm.
  bpmValue = bpm.getBpm();
  fprintf(stderr, "Done!\n");

  // rewind the file after bpm detection
  inFile->rewind();

  if (bpmValue > 0)
  {
    fprintf(stderr, "Detected BPM rate %.1f\n\n", bpmValue);
  }
  else
  {
    fprintf(stderr, "Couldn't detect BPM rate.\n\n");
    return;
  }

  if (params->goalBPM > 0)
  {
    // adjust tempo to given bpm
    params->tempoDelta = (params->goalBPM / bpmValue - 1.0f) * 100.0f;
    fprintf(stderr, "The file will be converted to %.1f BPM\n\n", params->goalBPM);
  }
}


int main(const int nParams, const char * const paramStr[])
{
  WavInFile *inFile;
  RunParameters *params = new RunParameters(nParams, paramStr);;
  struct ctx ctx;
  ctx.prog = CHP_BROWSE;
	ctx.midi_chan_override = -1;
	ctx.fx_chan = 0;
  ctx.active_rec_track = -1;
	ctx.num_pcm_handles = params->numHandles;
	if (ctx.num_pcm_handles > MAX_PCM_HANDLES){
		ctx.num_pcm_handles = MAX_PCM_HANDLES;
	}

	// greetings
  fprintf(stderr, _helloText, SoundTouch::getVersionString());

  try 
  {
		// Open pcm handles 
		for (int i = 0; i < ctx.num_pcm_handles; i++){
			ctx.pcm_handles[i].pcm = initPCM(SND_PCM_STREAM_PLAYBACK, params->pcmDevice);
		 if (ctx.pcm_handles[i].pcm == 0)
			 return -1;
		}
		ctx.pcm_cursor = 0;

		// open Midi port
    openMidi(&ctx);

    // Open input samples
    if (openFiles(&inFile, &ctx) != 0)
      return -1;

    // Setup the 'SoundTouch' object for processing the sound
    setup(&ctx.soundTouch);

    // Run controller 
    while (1) {
      readMidi(&ctx);
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


// set sample tempo to 120
// int tempoDelta = (120 / s->bpm - 1.0f) * 100.0f;
// printf("TEMPO DELLIETA! %d -> %d\n", s->bpm , tempoDelta);
//ctx->soundTouch.setTempoChange(tempoDelta);

