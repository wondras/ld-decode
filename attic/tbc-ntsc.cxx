/* LD decoder prototype, Copyright (C) 2013 Chad Page.  License: LGPL2 */

#include <complex>
#include "ld-decoder.h"
#include "deemp.h"
	
double clamp(double v, double low, double high)
{
        if (v < low) return low;
        else if (v > high) return high;
        else return v;
}

void aclamp(double *v, int len, double low, double high)
{
	for (int i = 0; i < len; i++) {
	        if (v[i] < low) v[i] = low;
		else if (v[i] > high) v[i] = high;
	}
}

// NTSC properties
#ifdef FSC10
const double in_freq = 10.0;	// in FSC.  Must be an even number!
#elif defined(FSC4)
const double in_freq = 4.0;	// in FSC.  Must be an even number!
#else
const double in_freq = 8.0;	// in FSC.  Must be an even number!
#endif

#define OUT_FREQ 4
const double out_freq = OUT_FREQ;	// in FSC.  Must be an even number!

struct VFormat {
	double cycles_line;
	double blanklen_ms;
	double a;	
};

double shift33 = (+.0 - (1 * (33.0 / 360.0))) * out_freq;

//const double ntsc_uline = 63.5; // usec_
const int ntsc_iplinei = 227.5 * in_freq; // pixels per line
const double ntsc_ipline = 227.5 * in_freq; // pixels per line
const double ntsc_opline = 227.5 * out_freq; // pixels per line

const double ntsc_blanklen = 9.2;

// we want the *next* colorburst as well for computation 
const double scale_linelen = ((63.5 + ntsc_blanklen) / 63.5); 

const double ntsc_ihsynctoline = ntsc_ipline * (ntsc_blanklen / 63.5);
const double iscale_tgt = ntsc_ipline + ntsc_ihsynctoline;

const double ntsc_hsynctoline = ntsc_opline * (ntsc_blanklen / 63.5);
const double scale_tgt = ntsc_opline + ntsc_hsynctoline;

double p_rotdetect = 40;

double hfreq = 525.0 * (30000.0 / 1001.0);

long long fr_count = 0, au_count = 0;

double f_tol = .5;

bool f_diff = false;

bool f_highburst = (in_freq == 4);
bool f_flip = false;
int writeonfield = 1;

bool audio_only = false;

double inscale = 327.68;
double inbase = (inscale * 20);	// IRE == -40

long long a_read = 0, v_read = 0;
int va_ratio = 80;

const int vblen = (ntsc_iplinei * 1100);	// should be divisible evenly by 16
const int ablen = (ntsc_iplinei * 1100) / 40;

const int absize = ablen * 8;
const int vbsize = vblen * 2;
	
float abuf[ablen * 2];
unsigned short inbuf[vblen];

inline double in_to_ire(uint16_t level)
{
	if (level == 0) return -100;
	
	return -40 + ((double)(level - inbase) / inscale); 
} 

inline uint16_t ire_to_in(double ire)
{
	if (ire <= -60) return 0;
	
	return clamp(((ire + 40) * inscale) + inbase, 1, 65535);
} 

inline uint16_t ire_to_out(double ire)
{
	if (ire <= -60) return 0;
	
	return clamp(((ire + 60) * 327.68) + 1, 1, 65535);
}

double out_to_ire(uint16_t in)
{
	return (in / 327.68) - 60;
}

inline double peakdetect_quad(double *y) 
{
	return (2 * (y[2] - y[0]) / (2 * (2 * y[1] - y[0] - y[2])));
}
	
// taken from http://www.paulinternet.nl/?page=bicubic
inline double CubicInterpolate(uint16_t *y, double x)
{
	double p[4];
	p[0] = y[0]; p[1] = y[1]; p[2] = y[2]; p[3] = y[3];

	return p[1] + 0.5 * x*(p[2] - p[0] + x*(2.0*p[0] - 5.0*p[1] + 4.0*p[2] - p[3] + x*(3.0*(p[1] - p[2]) + p[3] - p[0])));
}

inline void Scale(uint16_t *buf, double *outbuf, double start, double end, double outlen, double offset = 0)
{
	double inlen = end - start;
	double perpel = inlen / outlen; 

	double p1 = start + (offset * perpel);
	for (int i = 0; i < outlen; i++) {
		int index = (int)p1;
		if (index < 1) index = 1;

		outbuf[i] = clamp(CubicInterpolate(&buf[index - 1], p1 - index), 0, 65535);

		p1 += perpel;
	}
}
                
bool InRange(double v, double l, double h) {
	return ((v > l) && (v < h));
}

bool InRangeCF(double v, double l, double h) {
	l *= in_freq;
	h *= in_freq;
	return ((v > l) && (v < h));
}

// tunables
bool freeze_frame = false;
bool despackle = true;
int afd = -1, fd = 0;

double black_ire = 7.5;

int write_locs = -1;

uint16_t frame[505][(int)(OUT_FREQ * 211)];
uint16_t frame_orig[505][(int)(OUT_FREQ * 211)];
double Δframe[505][(int)(OUT_FREQ * 211)];
double Δframe_filt[505][(int)(OUT_FREQ * 211)];

#ifdef FSC10
Filter f_longsync(f_dsync10);
Filter f_syncid(f_syncid10);
int syncid_offset = syncid10_offset;
#elif defined(FSC4)
Filter f_longsync(f_dsync4);
Filter f_syncid(f_syncid4);
int syncid_offset = syncid4_offset; 
#else
Filter f_longsync(f_dsync);
Filter f_syncid(f_syncid8);
int syncid_offset = syncid8_offset;
#endif

bool BurstDetect_New(double *line, int freq, double _loc, bool tgt, double &plevel, double &pphase) 
{
	int len = (28 * freq);
	int loc = _loc * freq;
	int count = 0, cmin = 0;
	double ptot = 0, tpeak = 0, tmin = 0;
	double start = 15;

	double phase = 0;

//	cerr << ire_to_in(7) << ' ' << ire_to_in(16) << endl;
	double highmin = ire_to_in(f_highburst ? 12 : 7);
	double highmax = ire_to_in(f_highburst ? 23 : 22);
	double lowmin = ire_to_in(f_highburst ? -12 : -7);
	double lowmax = ire_to_in(f_highburst ? -23 : -22);
//	cerr << lowmin << ' ' << lowmax << endl;

	if (f_highburst) {
		start = 20;
		len = (start + 6) * freq;
	}

	for (int i = loc + (start * freq); i < loc + len; i++) {
		if ((line[i] > highmin) && (line[i] < highmax) && (line[i] > line[i - 1]) && (line[i] > line[i + 1])) {
			double c = round(((i + peakdetect_quad(&line[i - 1])) / 4) + (tgt ? 0.5 : 0)) * 4;

//			cerr << "B " << i + peakdetect_quad(&line[i - 1]) << ' ' << c << endl;

			if (tgt) c -= 2;
			phase = (i + peakdetect_quad(&line[i - 1])) - c;

			ptot += phase;

			tpeak += line[i];

			count++;
//			cerr << "BDN " << i << ' ' << in_to_ire(line[i]) << ' ' << line[i - 1] << ' ' << line[i] << ' ' << line[i + 1] << ' ' << phase << ' ' << (i + peakdetect_quad(&line[i - 1])) << ' ' << c << ' ' << ptot << endl; 
		} 
		else if ((line[i] < lowmin) && (line[i] > lowmax) && (line[i] < line[i - 1]) && (line[i] < line[i + 1])) {
			cmin++;
			tmin += line[i];
		}
	}

	plevel = ((tpeak / count) - (tmin / cmin)) / 4.2;
	pphase = (ptot / count) * 1;

//	cerr << "BDN end " << plevel << ' ' << pphase << ' ' << count << endl;
	
	return (count >= 3);
}

int get_oline(double line)
{
	int l = (int)line;

	if (l < 10) return -1;
	else if (l < 263) return (l - 10) * 2;
	else if (l < 273) return -1;
	else if (l < 525) return ((l - 273) * 2) + 1;

	return -1;
}

bool is_visibleline(int line) 
{
	return (get_oline(line) >= 22);
}

double pleft = 0, pright = 0;
Filter f_fml(f_fmdeemp), f_fmr(f_fmdeemp);

uint16_t aout[512];
int aout_i = 0;
void ProcessAudioSample(float left, float right, double vel)
{
	left = f_fml.feed(left * (65535.0 / 300000.0));
	left += 32768;
	
	right = f_fmr.feed(right * (65535.0 / 300000.0));
	right += 32768;

	aout[aout_i * 2] = clamp(left, 0, 65535);
	aout[(aout_i * 2) + 1] = clamp(right, 0, 65535);
	
	aout_i++;
	if (aout_i == 256) {
		int rv = write(audio_only ? 1 : 3, aout, sizeof(aout));

		rv = aout_i = 0;
	}
}

double afreq = 48000;

double prev_time = -1;
double next_audsample = 0;
size_t prev_loc = -1;

long long prev_index = 0, prev_i = 0;
void ProcessAudio(double frame, long long loc, float *abuf)
{
	double time = frame / (30000.0 / 1001.0);

	cerr << "PA " << frame << ' ' << loc << endl;

	if (afd < 0) return;

	if (prev_time >= 0) {
		while (next_audsample < time) {
			double i1 = (next_audsample - prev_time) / (time - prev_time); 
			long long i = (i1 * (loc - prev_loc)) + prev_loc;

			if (i < v_read) {
				ProcessAudioSample(f_fml.val(), f_fmr.val(), 1.0);  
			} else {
				long long index = (i / va_ratio) - a_read;
				if (index >= ablen) {
					cerr << "audio error " << frame << " " << time << " " << i1 << " " << i << " " << index << " " << ablen << endl;
					index = ablen - 1;
//					exit(0);
				} 
				float left = abuf[index * 2], right = abuf[(index * 2) + 1];
				cerr << "A " << frame << ' ' << loc << ' ' << i1 << ' ' << i << ' ' << i - prev_i << ' ' << index << ' ' << index - prev_index << ' ' << left << ' ' << right << endl;
				prev_index = index;
				prev_i = i;
				ProcessAudioSample(left, right, 1.0);
			} 

			next_audsample += 1.0 / afreq;
		}
	}

	prev_time = time; prev_loc = loc;
}

double tline = 0, line = -2;
int phase = -1;

bool first = true;
double prev_linelen = ntsc_ipline;
double prev_offset = 0.0;

double prev_begin = 0, prev_end = 0;
double prev_beginlen = 0, prev_endlen = 0;

double prev_lvl_adjust = 1.0;

int iline = 0;
int frameno = -1;

static int offburst = 0;

struct Line {
	double center;
	double peak;
	double beginsync, endsync;
	int linenum;
	bool bad;
};

void HandleBadLine(vector<Line> &peaks, int i)
{			
	int line = peaks[i].linenum;
	cerr << "BAD " << i << ' ' << line << ' ';
	cerr << peaks[i].beginsync << ' ' << peaks[i].center << ' ' << peaks[i].endsync << ' ' << peaks[i].endsync - peaks[i].beginsync << endl;

	int lg = 1;

	for (lg = 1; lg < 8 && ((i - lg) >= 0) && ((i + lg) < peaks.size()) && (peaks[i - lg].bad || peaks[i + lg].bad); lg++);

	cerr << peaks[i-lg].beginsync << ' ' << peaks[i-lg].center << ' ' << peaks[i-lg].endsync << ' ' << peaks[i-lg].endsync - peaks[i-lg].beginsync << endl;
	cerr << "BADLG " << lg << ' ';
	double gap = (peaks[i + lg].beginsync - peaks[i - lg].beginsync) / (lg * 2);
	peaks[i].beginsync = peaks[i - lg].beginsync + (gap * lg); 
	peaks[i].center = peaks[i - lg].center + (gap * lg); 
	peaks[i].endsync = peaks[i - lg].endsync + (gap * lg); 
	cerr << peaks[i].beginsync << ' ' << peaks[i].center << ' ' << peaks[i].endsync << ' ' << peaks[i].endsync - peaks[i].beginsync << endl;
	cerr << peaks[i+lg].beginsync << ' ' << peaks[i+lg].center << ' ' << peaks[i+lg].endsync << ' ' << peaks[i+lg].endsync - peaks[i+lg].beginsync << endl;
}


double ProcessLine(uint16_t *buf, vector<Line> &lines, int index, bool recurse = false)
{
	double begin = lines[index - 1].beginsync;
	double end = lines[index - 1].beginsync + ((lines[index].beginsync - lines[index - 1].beginsync) * scale_linelen);
	bool err = lines[index].bad;
	int line = lines[index].linenum;
	double tout[8192];
	double adjlen = ntsc_ipline;
	int pass = 0;

	double plevel1, plevel2;
	double nphase1, nphase2;

	if (begin < 0) begin = 0;

	cerr << "PL " << line << ' ' << begin << ' ' << end << ' ' << err << ' ' << end - begin << endl;

	double orig_begin = begin;
	double orig_end = end;

	int oline = get_oline(line);

	if (oline < 0) return 0;

	double tgt_nphase = 0;
	double nadj1 = 1, nadj2 = 1;

	cerr << "ProcessLine " << begin << ' ' << end << endl;

	Scale(buf, tout, begin, end, scale_tgt); 
	
	if (phase != -1) {
		tgt_nphase = ((line + phase + iline) % 2) ? -2 : 0;
	} 

	bool valid = BurstDetect_New(tout, out_freq, 0, tgt_nphase != 0, plevel1, nphase1); 
	valid &= BurstDetect_New(tout, out_freq, 228, tgt_nphase != 0, plevel2, nphase2); 

	cerr << "levels " << plevel1 << ' ' << plevel2 << " valid " << valid << endl;

	if (!valid || (plevel1 < (f_highburst ? 1800 : 1000)) || (plevel2 < (f_highburst ? 1000 : 800))) {
		begin += prev_offset;
		end += prev_offset;
	
		Scale(buf, tout, begin, end, scale_tgt, shift33); 
		goto wrapup;
	}

	if (err) {
		begin += prev_offset;
		end += prev_offset;
	}

	if ((phase == -1) || (offburst >= 5)) {
		phase = (fabs(nphase1) > 1);
		iline = line;
		offburst = 0;
		cerr << "p " << nphase1 << ' ' << phase << endl;

		tgt_nphase = ((line + phase + iline) % 2) ? -2 : 0;
	} 

	adjlen = (end - begin) / (scale_tgt / ntsc_opline);
	//cerr << line << " " << oline << " " << tgt_nphase << ' ' << begin << ' ' << (begin + adjlen) << '/' << end  << endl;

	for (pass = 0; (pass < 12) && ((fabs(nadj1) + fabs(nadj2)) > .05); pass++) {
		nadj1 = nphase1 * 1;
		nadj2 = nphase2 * 1;

		begin += nadj1;
		if (pass) end += nadj2;

		Scale(buf, tout, begin, end, scale_tgt); 
		BurstDetect_New(tout, out_freq, 0, tgt_nphase != 0, plevel1, nphase1); 
		BurstDetect_New(tout, out_freq, 228, tgt_nphase != 0, plevel2, nphase2); 
		
		nadj1 = (nphase1) * 1;
		nadj2 = (nphase2) * 1;

		adjlen = (end - begin) / (scale_tgt / ntsc_opline);
	}
	
	if (!recurse) {
		//double prev_len = prev_end - prev_begin;
		double orig_len = orig_end - orig_begin;
		double new_len = end - begin;

		double beginlen = begin - prev_begin;
		double endlen = end - prev_end;

		cerr << "len " << frameno + 1 << ":" << oline << ' ' << orig_len << ' ' << new_len << ' ' << orig_begin << ' ' << begin << ' ' << orig_end << ' ' << end << endl;
		if ((fabs(prev_endlen - endlen) > (out_freq * f_tol)) || (fabs(prev_beginlen - beginlen) > (out_freq * f_tol))) {
			//cerr << "ERRP len " << frameno + 1 << ":" << oline << ' ' << orig_len << ' ' << new_len << ' ' << orig_begin << ' ' << begin << ' ' << orig_end << ' ' << end << endl;
			cerr << "ERRP len " << frameno + 1 << ":" << oline << ' ' << prev_endlen - endlen << ' ' << prev_beginlen - beginlen << endl;

			if (oline > 20) lines[index].bad = true;
			HandleBadLine(lines, index);
			return ProcessLine(buf, lines, index, true);
		}
	}
		
	Scale(buf, tout, begin, end, scale_tgt, shift33); 
	
	cerr << "final levels " << plevel1 << ' ' << plevel2 << endl;

	// trigger phase re-adjustment if we keep adjusting over 3 pix/line
	if (fabs(begin - orig_begin) > (in_freq * .375)) {
		offburst++;
	} else {
		offburst = 0;
	}

wrapup:
	// LD only: need to adjust output value for velocity, and remove defects as possible
	double lvl_adjust = ((((end - begin) / iscale_tgt) - 1) * 1.0) + 1;
	
	if (lines[index].bad) {
		lvl_adjust = prev_lvl_adjust;
	} else {
		prev_lvl_adjust = lvl_adjust;
	}

	double prev_o = 0;
	for (int h = 0; (oline > 2) && (h < (211 * out_freq)); h++) {
		double v = tout[h + (int)(15 * out_freq)];
		double ire = in_to_ire(v);
		double o;

		if (in_freq != 4) {
			double freq = (ire * ((9300000 - 8100000) / 100)) + 8100000; 

			freq *= lvl_adjust;
			ire = ((freq - 8100000) / 1200000) * 100;
			o = ire_to_out(ire);
		} else { 
			o = ire_to_out(in_to_ire(v));
		}

		frame[oline][h] = clamp(o, 0, 65535);

		Δframe[oline][h] = fabs(o - prev_o);
		double dfilt = f_lp18.feed(Δframe[oline][h]); 
		if (h > 12) {
		//	cerr << "RD " << oline << ' ' << h << ' ' << Δframe[oline][h - 12] << ' ' << dfilt << endl;
//			if (dfilt < 0) dfilt = 0;
			Δframe_filt[oline][h - 12] = dfilt;
		}	
		prev_o = o;
	}

        if (!pass) {
                for (int x = 2; x < 6; x++) {frame[oline][x] = 32000;}
		cerr << "BURST ERROR " << line << " " << pass << ' ' << begin << ' ' << (begin + adjlen) << '/' << end  << ' ' << endl;
        } else {
		prev_offset = begin - orig_begin;
	}

	cerr << line << " GAP " << begin - prev_begin << ' ' << prev_begin << ' ' << begin << endl;
	
	frame[oline][0] = (tgt_nphase != 0) ? 32768 : 16384; 
	frame[oline][1] = plevel1; 

	prev_beginlen = begin - prev_begin; 
	prev_endlen = end - prev_end; 

	prev_begin = begin;
	prev_end = end;


	return adjlen;
}

//uint16_t synclevel = 12000;
uint16_t synclevel = inbase + (inscale * 15);

bool IsPeak(double *p, int i)
{
	return (fabs(p[i]) >= fabs(p[i - 1])) && (fabs(p[i]) >= fabs(p[i + 1]));
}

// Essential VBI/Phillips code reference: http://www.daphne-emu.com/mediawiki/index.php/VBIInfo

// (LD-V6000A info page is cryptic but very essential!)

double dots_usec = 4.0 * 315.0 / 88.0;
uint32_t ReadPhillipsCode(uint16_t *line) {
	int first_bit = -1; // 108 - dots_usec;
	uint32_t out = 0;

	double Δline[844];

	for (int i = 1; i < 843; i++) {
		Δline[i] = line[i] - line[i - 1]; 
	}

	// find first positive transition (exactly halfway into bit 0 which is *always* 1) 
	for (int i = 70; (first_bit == -1) && (i < 140); i++) {
//		cerr << i << ' ' << out_to_ire(line[i]) << ' ' << Δline[i] << endl;
		if (IsPeak(Δline, i) && (Δline[i] > 10 * 327.68)) {
			first_bit = i; 
		}
	}
	if (first_bit < 0) return 0;

	for (int i = 0; i < 24; i++) {
		int rloc = -1, loc = (first_bit + (i * 2 * dots_usec));
		double rpeak = -1;

		for (int h = loc - 8; (h < loc + 8); h++) {
			if (IsPeak(Δline, h)) {
				if (fabs(Δline[h]) > rpeak) {
					rpeak = fabs(Δline[h]);
					rloc = h;
				}
			}
		}

		if (rloc == -1) rloc = loc;

		out |= (Δline[rloc] > 0) ? (1 << (23 - i)) : 0;
		cerr << i << ' ' << loc << ' ' << Δline[loc] << ' ' << rloc << ' ' << Δline[rloc] << ' ' << Δline[rloc] / inscale << ' ' << out << endl; 

		if (!i) first_bit = rloc;
	}
	cerr << "P " << hex << out << dec << endl;			

	return out;
}

const int out_x = 844;
const int out_y = 505;

inline double max(double a, double b)
{
	return (a > b) ? a : b;
}

void Despackle()
{
	memcpy(frame_orig, frame, sizeof(frame));

	for (int y = 22; y < out_y; y++) {
		double rotdetect = p_rotdetect * inscale;

		for (int x = 60; x < out_x - 16; x++) {

			double comp = 0;
	
			for (int cy = y - 1; (cy < (y + 2)) && (cy < out_y); cy++) { 
				for (int cx = x - 3; (cx < x + 3) && (cx < (out_x - 12)); cx++) {
					comp = max(comp, Δframe_filt[cy][cx]);
//					cerr << "RD " << cy << ' ' << cx << ' ' << comp << ' ' << Δframe_filt[cy][cx] << endl; 
				}
			}

//			if ((Δframe[y][x] > rotdetect)) {
			if ((out_to_ire(frame[y][x]) < -20) || (out_to_ire(frame[y][x]) > 140) || ((Δframe[y][x] > rotdetect) && ((Δframe[y][x] - comp) > rotdetect))) {
//			if (((Δframe[y][x] > rotdetect) && ((Δframe[y][x] - comp) > rotdetect))) {
				cerr << "R " << y << ' ' << x << ' ' << rotdetect << ' ' << Δframe[y][x] << ' ' << comp << ' ' << Δframe_filt[y][x] << endl;
				for (int m = x - 4; (m < (x + 14)) && (m < out_x); m++) {
					double tmp = (((double)frame_orig[y - 2][m - 2]) + ((double)frame_orig[y - 2][m + 2])) / 2;

					if (y < (out_y - 3)) {
						tmp /= 2;
						tmp += ((((double)frame_orig[y + 2][m - 2]) + ((double)frame_orig[y + 2][m + 2])) / 4);
					}

//					cerr << "Z " << y << ' ' << x << ' ' << m << endl;
					frame[y][m] = clamp(tmp, 0, 65535);
				}
				x = x + 14;
			}
		} 
	}
}

bool CheckWhiteFlag(int l)
{
	int wc = 0;

	for (int i = 100; i < 800; i++) {
		if (out_to_ire(frame[l][i]) > 80) wc++;
		if (wc >= 200) return true;
 	}

	return false;
}

void DecodeVBI()
{
	uint32_t code[6];

	uint32_t clv_time = 0;
	uint32_t chap = 0;
	uint32_t flags = 0;

	bool odd = false; // CAV framecode on odd scanline
	bool even = false; // on even scanline (need to report both, since some frames have none!)
	bool clv = false;
	bool cx  = false;
	int fnum = 0;

	memset(code, 0, sizeof(code));
	for (int i = 14; i < 20; i++) {
		code[i - 14] = ReadPhillipsCode(frame[i]);
	}
	cerr << "Phillips codes " << hex << code[0] << ' ' << code[1] << ' ' << code[2] << ' ' << code[3] << ' ' << code[4] << ' ' << code[5] << dec << endl;

	for (int i = 0; i < 6; i++) {
		frame[0][i * 2] = code[i] >> 16;
		frame[0][(i * 2) + 1] = code[i] & 0xffff;
		
		if ((code[i] & 0xf00fff) == 0x800fff) {
			chap =  ((code[i] & 0x00f000) >> 12);
			chap += (((code[i] & 0x0f0000) >> 16) - 8) * 10;
		}

		if ((code[i] & 0xfff000) == 0x8dc000) {
			cx = true;
		}

		if (0x87ffff == code[i]) {
			clv = true;
		}
	}

	if (clv == true) {
		uint16_t hours = 0;
		uint16_t minutes = 0;
		uint16_t seconds = 0;
		uint16_t framenum = 0;
		// Find CLV frame # data
		for (int i = 0; i < 6; i++) {
			// CLV Picture #
			if (((code[i] & 0xf0f000) == 0x80e000) && ((code[i] & 0x0f0000) >= 0x0a0000)) {
				seconds = (((code[i] & 0x0f0000) - 0x0a0000) >> 16) * 10; 
				seconds += (code[i] & 0x000f00) >> 8; 
				framenum = code[i] & 0x0f;
				framenum += ((code[i] & 0x000f0) >> 4) * 10;
			} 
			if ((code[i] & 0xf0ff00) == 0xf0dd00) {
				hours = ((code[i] & 0x0f0000) >> 16);
				minutes = code[i] & 0x0f;
				minutes += ((code[i] & 0x000f0) >> 4) * 10;
			} 
		}
		fnum = (((hours * 3600) + (minutes * 60) + seconds) * 30) + framenum; 
		clv_time = (hours << 24) | (minutes << 16) || (seconds << 8) || framenum;
		cerr << "CLV " << hours << ':' << minutes << ':' << seconds << '.' << framenum << endl;
	} else {
		for (int i = 0; i < 6; i++) {
			// CAV frame:  f80000 + frame
			if ((code[i] >= 0xf80000) && (code[i] <= 0xffffff)) {
				// Convert from BCD to binary
				fnum = code[i] & 0x0f;
				fnum += ((code[i] & 0x000f0) >> 4) * 10;
				fnum += ((code[i] & 0x00f00) >> 8) * 100;
				fnum += ((code[i] & 0x0f000) >> 12) * 1000;
				fnum += ((code[i] & 0xf0000) >> 16) * 10000;
				if (fnum >= 80000) fnum -= 80000;
				cerr << i << " CAV frame " << fnum << endl;
				if (i % 2) odd = true;
				if (!(i % 2)) even = true;
			} 
		}	
	}	
	cerr << " fnum " << fnum << endl;

	flags = (clv ? FRAME_INFO_CLV : 0) | (even ? FRAME_INFO_CAV_EVEN : 0) | (odd ? FRAME_INFO_CAV_ODD : 0) | (cx ? FRAME_INFO_CX : 0); 
	flags |= CheckWhiteFlag(4) ? FRAME_INFO_WHITE_EVEN : 0; 
	flags |= CheckWhiteFlag(5) ? FRAME_INFO_WHITE_ODD  : 0; 

	cerr << "Status " << hex << flags << dec << " chapter " << chap << endl;

	frame[0][12] = chap;
	frame[0][13] = flags;
	frame[0][14] = fnum >> 16;
	frame[0][15] = fnum & 0xffff;
	frame[0][16] = clv_time >> 16;
	frame[0][17] = clv_time & 0xffff;
//	exit(0);
}

double psync[ntsc_iplinei*1200];
int Process(uint16_t *buf, int len, float *abuf, int alen)
{
	vector<Line> peaks; 
	peaks.clear();

	memset(frame, 0, sizeof(frame));

	// clear line length filter - will be repopulated with the pre-line 22/273 samples	
	f_linelen.clear(ntsc_ipline);

	// sample syncs
	f_syncid.clear(0);

	for (int i = 0; i < len; i++) {
		double val = f_syncid.feed(buf[i] && (buf[i] < synclevel)); 
		if (i > syncid_offset) psync[i - syncid_offset] = val; 
	}

	for (int i = 0; i < len - syncid_offset; i++) {
		double level = psync[i];

		if ((level > .05) && (level > psync [i - 1]) && (level > psync [i + 1])) {
			Line l;

			l.center = i;
			l.peak   = level;
			l.bad = false;
			l.linenum = -1;

			peaks.push_back(l);	
			cerr << peaks.size() << ' ' << i << ' ' << level << endl;

		}
	}

	// PASS 1: find first field index - returned as firstline 
	int firstpeak = -1, firstline = -1, lastline = -1;
	for (int i = 9; (i < peaks.size() - 9) && (firstline == -1); i++) {
		if (peaks[i].peak > 1.0) {
			if (peaks[i].center > (ntsc_ipline * 400)) {
				//return (ntsc_ipline * 350);
				return (peaks[i].center - (ntsc_ipline * 20));
			} else if (peaks[i].center < (ntsc_ipline * 8)) {
				return (ntsc_ipline * 400);
			} else {
				firstpeak = i;
				firstline = -1; lastline = -1;

				//cerr << firstpeak << ' ' << peaks[firstpeak].peak << ' ' << peaks[firstpeak].center << endl;
	
				for (int i = firstpeak - 1; (i > 0) && (lastline == -1); i--) {
					if ((peaks[i].peak > 0.2) && (peaks[i].peak < 0.75)) lastline = i;
				}	

				int distance_prev = peaks[lastline + 1].center - peaks[lastline].center;
				int synctype = (distance_prev > (in_freq * 140)) ? 1 : 2;

				cerr << "P1_" << lastline << ' ' << synctype << ' ' << (in_freq * 140) << ' ' << distance_prev << ' ' << peaks[lastline + 1].center - peaks[lastline].center << endl;
	
				for (int i = firstpeak + 1; (i < peaks.size()) && (firstline == -1); i++) {
					if ((peaks[i].peak > 0.2) && (peaks[i].peak < 0.75)) firstline = i;
				}	
	
				cerr << firstline << ' ' << peaks[firstline].center - peaks[firstline-1].center << endl;

				cerr << synctype << ' ' << writeonfield << endl;
				if (synctype != writeonfield) {
					firstline = firstpeak = -1; 
					i += 6;
				}
			}
		}
	}

	// PASS 2: Line processing and error detection
	bool field2 = false;
	int line = -10;
	double prev_linelen = 1820;

	for (int i = firstline - 1; i < peaks.size() && (i < (firstline + 540)) && (line < 526); i++) {
//		cerr << "P2A " << i << ' ' << peaks[i].peak << endl;
		bool canstartsync = false;
		if ((line < 0) || InRange(line, 262, 274) || InRange(line, 524, 530)) canstartsync = true;

		if (!canstartsync && ((peaks[i].center - peaks[i - 1].center) > (440 * in_freq)) && (peaks[i].center > peaks[i - 1].center)) {
			// looks like we outright skipped a line because of corruption.  add a new one! 
			cerr << "LONG " << i << ' ' << peaks[i].center << ' ' << peaks[i].center - peaks[i - 1].center << ' ' << peaks.size() << endl ;

			Line l;

			l.center = peaks[i - 1].center + 1820;
			l.peak   = peaks[i - 1].peak;
			l.bad = true;
			l.linenum = -1;

			peaks.insert(peaks.begin()+i, l);

			i--;
			line--;
		} else if (!canstartsync && ((peaks[i].center - peaks[i - 1].center) < (207.5 * in_freq)) && (peaks[i].center > peaks[i - 1].center)) {
			// recovery routine for if we get a short line due to excessive dropout
 
			cerr << "SHORT " << i << ' ' << peaks[i].center << ' ' << peaks[i].center - peaks[i - 1].center << ' ' << peaks.size() << endl ;

			peaks.erase(peaks.begin()+i);
			i--;
//			cerr << "ohoh." << i << ' ' << peaks.size() << endl ;
			line--;
		} else if (peaks[i].peak > .85) {
			line = -10;
			peaks[i].linenum = -1;
		} else if (InRange(peaks[i].peak, canstartsync ? .25 : .0, (line < 0) ? .50 : .8)) {
			int cbeginsync = 0, cendsync = 0;
			int center = peaks[i].center;

			// XXX:  This can be fragile
			if (line <= -1) {
				line = field2 ? 273 : 10;
				field2 = true;
			}

			peaks[i].beginsync = peaks[i].endsync = -1;
			for (int x = 0; x < 200 && InRange(peaks[i].peak, .20, .5) && ((peaks[i].beginsync == -1) || (peaks[i].endsync == -1)); x++) {
				cbeginsync++;
				cendsync++;
	
				if (buf[center - x] < synclevel) cbeginsync = 0;
				if (buf[center + x] < synclevel) cendsync = 0;

				if ((cbeginsync == 4) && (peaks[i].beginsync < 0)) peaks[i].beginsync = center - x + 4;			
				if ((cendsync == 4) && (peaks[i].endsync < 0)) peaks[i].endsync = center + x - 4;			
			}

			peaks[i].bad = !InRangeCF(peaks[i].endsync - peaks[i].beginsync, 15.5, 18.5);

			double prev_linelen_cf = clamp(prev_linelen / in_freq, 226, 229);

			if (!peaks[i - 1].bad) peaks[i].bad |= get_oline(line) > 22 && (!InRangeCF(peaks[i].beginsync - peaks[i-1].beginsync, prev_linelen_cf - f_tol, prev_linelen_cf + f_tol) || !InRangeCF(peaks[i].endsync - peaks[i-1].endsync, prev_linelen_cf - f_tol, prev_linelen_cf + f_tol)); 
			peaks[i].linenum = line;
			
			cerr << "P2_" << line << ' ' << i << ' ' << peaks[i].bad << ' ' <<  peaks[i].peak << ' ' << peaks[i].center << ' ' << prev_linelen_cf << ' ' << peaks[i].center - peaks[i-1].center << ' ' << peaks[i].beginsync << ' ' << peaks[i].endsync << ' ' << peaks[i].endsync - peaks[i].beginsync << endl;
				
			// HACK!
			if (line == 273) peaks[i].linenum = -1;

			// if we have a good line, feed it's length to the line LPF.  The 8 line lag is insignificant 
			// since it's a ~30hz oscillation. 
			double linelen = peaks[i].center - peaks[i-1].center;
			if (!peaks[i].bad && !peaks[i - 1].bad && InRangeCF(linelen, 227.5 - 4, 227.5 + 4)) {
				prev_linelen = f_linelen.feed(linelen);
			}
		}
		line++;
	}

	// PASS 3:  Error correction
	line = -1;	
	for (int i = firstline - 1; (i < (firstline + 540)) && (line < 526); i++) {
		if (peaks[i].linenum > 0 && peaks[i].bad && (get_oline(peaks[i].linenum) >= 22)) {
			HandleBadLine(peaks, i);
		}
	}

	// PASS 3:  Wrapup and audio processing 
	line = -1;	
	for (int i = firstline - 1; (i < (firstline + 540)) && (line < 526); i++) {
		if ((peaks[i].linenum > 0) && (peaks[i].linenum <= 525)) {
			line = peaks[i].linenum ;
			cerr << line << ' ' << i << ' ' << peaks[i].bad << ' ' <<  peaks[i].peak << ' ' << peaks[i].center << ' ' << peaks[i].center - peaks[i-1].center << ' ' << peaks[i].beginsync << ' ' << peaks[i].endsync << ' ' << peaks[i].endsync - peaks[i].beginsync << endl;
				
			// XXX:  This is a hack to avoid a crashing condition!
			if (!((line < 12) && (peaks[i].center - peaks[i-1].center) < (in_freq * 200))) {
				ProcessLine(buf, peaks, i); 

				cerr << "PA " << (line / 525.0) + frameno << ' ' << v_read + peaks[i].beginsync << endl;
				ProcessAudio((line / 525.0) + frameno, v_read + peaks[i].beginsync, abuf); 
				
				if (peaks[i].bad) {
					int oline = get_oline(line);
               		 		frame[oline][3] = frame[oline][5] = 65000;
			                frame[oline][4] = frame[oline][6] = 0;
				}
			}
		}
	}

	if (despackle) Despackle();

	// Decode VBI data
	DecodeVBI();

	frameno++;
	cerr << "WRITING\n";
	write(1, frame, sizeof(frame));
	memset(frame, 0, sizeof(frame));

	if (!freeze_frame && phase >= 0) phase = !phase;
	
	return peaks[firstline + 500].center;
}

bool seven_five = (in_freq == 4);
double low = 65535, high = 0;

double f[vblen];

void autoset(uint16_t *buf, int len, bool fullagc = true)
{
	int lowloc = -1;
	int checklen = (int)(in_freq * 4);

	if (!fullagc) {
		low = 65535;
		high = 0;
	}

	cerr << "old base:scale = " << inbase << ':' << inscale << endl;
	
//	f_longsync.clear(0);

	// phase 1:  get low (-40ire) and high (??ire)
	for (int i = 0; i < len; i++) {
		f[i] = f_longsync.feed(buf[i]);

		if ((i > (in_freq * 256)) && (f[i] < low) && (f[i - checklen] < low)) {
			if (f[i - checklen] > f[i]) 
				low = f[i - checklen];
			else 
				low = f[i];

			lowloc = i;
		}
		
		if ((i > (in_freq * 256)) && (f[i] > high) && (f[i - checklen] > high)) {
			if (f[i - checklen] < f[i]) 
				high = f[i - checklen];
			else 
				high = f[i];
		}

//		cerr << i << ' ' << buf[i] << ' ' << f[i] << ' ' << low << ':' << high << endl;
	}

//	cerr << lowloc << ' ' << low << ':' << high << endl;

	// phase 2: attempt to figure out the 0IRE porch near the sync

	if (!fullagc) {
		int gap = high - low;
		int nloc;

		for (nloc = lowloc; (nloc > lowloc - (in_freq * 320)) && (f[nloc] < (low + (gap / 8))); nloc--);

		cerr << nloc << ' ' << (lowloc - nloc) / in_freq << ' ' << f[nloc] << endl;

		nloc -= (in_freq * 4);
		cerr << nloc << ' ' << (lowloc - nloc) / in_freq << ' ' << f[nloc] << endl;
	
		cerr << "old base:scale = " << inbase << ':' << inscale << endl;

		inscale = (f[nloc] - low) / ((seven_five) ? 47.5 : 40.0);
		inbase = low - (20 * inscale);	// -40IRE to -60IRE
		if (inbase < 1) inbase = 1;
		cerr << "new base:scale = " << inbase << ':' << inscale << endl;
	} else {
		inscale = (high - low) / 140.0;
	}

	inbase = low;	// -40IRE to -60IRE
	if (inbase < 1) inbase = 1;

	cerr << "new base:scale = " << inbase << ':' << inscale << " low: " << low << ' ' << high << endl;

	synclevel = inbase + (inscale * 20);
}

int main(int argc, char *argv[])
{
	int rv = 0, arv = 0;
	bool do_autoset = (in_freq == 4);
	long long dlen = -1;
	unsigned char *cinbuf = (unsigned char *)inbuf;
	unsigned char *cabuf = (unsigned char *)abuf;

	int c;

	cerr << std::setprecision(10);

	opterr = 0;
	
	while ((c = getopt(argc, argv, "dHmhgs:n:i:a:AfFt:r:")) != -1) {
		switch (c) {
			case 'd':	// show differences between pixels
				f_diff = true;
				break;
			case 'm':	// "magnetic video" mode - bottom field first
				writeonfield = 2;
				break;
			case 'F':	// flip fields 
				f_flip = true;
				break;
			case 'i':
				fd = open(optarg, O_RDONLY);
				break;
			case 'a':
				afd = open(optarg, O_RDONLY);
				break;
			case 'A':
				audio_only = true;
				break;
			case 'g':
				do_autoset = !do_autoset;
				break;
			case 'n':
				despackle = false;
				break;
			case 'f':
				freeze_frame = true;
				break;
			case 'h':
				seven_five = true;
				break;
			case 'H':
				f_highburst = !f_highburst;
				break;
			case 't':
				sscanf(optarg, "%lf", &f_tol);		
				break;
			case 'r':
				sscanf(optarg, "%lf", &p_rotdetect);		
				break;
			default:
				return -1;
		} 
	} 

	cerr << "freq = " << in_freq << endl;

	rv = read(fd, inbuf, vbsize);
	while ((rv > 0) && (rv < vbsize)) {
		int rv2 = read(fd, &cinbuf[rv], vbsize - rv);
		if (rv2 <= 0) exit(0);
		rv += rv2;
	}

	cerr << "B" << absize << ' ' << ablen * 2 * sizeof(float) << endl ;

	if (afd != -1) {	
		arv = read(afd, abuf, absize);
		while ((arv > 0) && (arv < absize)) {
			int arv2 = read(afd, &cabuf[arv], absize - arv);
			if (arv2 <= 0) exit(0);
			arv += arv2;
		}
	}
							
	memset(frame, 0, sizeof(frame));

	f_linelen.clear(ntsc_ipline);

	size_t aplen = 0;
	while (rv == vbsize && ((v_read < dlen) || (dlen < 0))) {
		if (do_autoset) {
			autoset(inbuf, vbsize / 2);
		}

		int plen;

		plen = Process(inbuf, rv / 2, abuf, arv / 8);
	
		if (plen < 0) {
			cerr << "skipping ahead" << endl;
			plen = vblen / 2;
		}

		v_read += plen;
		aplen = (v_read / va_ratio) - a_read;

		a_read += aplen;

//		cerr << "move " << plen << ' ' << (vblen - plen) * 2; 
		memmove(inbuf, &inbuf[plen], (vblen - plen) * 2);
	
                rv = read(fd, &inbuf[(vblen - plen)], plen * 2) + ((vblen - plen) * 2);
		while ((rv > 0) && (rv < vbsize)) {
			int rv2 = read(fd, &cinbuf[rv], vbsize - rv);
			if (rv2 <= 0) exit(0);
			rv += rv2;
		}	
		
		if (afd != -1) {	
			cerr << "AA " << plen << ' ' << aplen << ' ' << v_read << ' ' << a_read << ' ' << (double)v_read / (double)a_read << endl;
			memmove(abuf, &abuf[aplen * 2], absize - (aplen * 8));
			cerr << abuf[0] << endl;

			arv = (absize - (aplen * 8));
			while (arv < absize) {
				int arv2 = read(afd, &cabuf[arv], absize - arv);
				if (arv2 <= 0) exit(0);
				arv += arv2;
			}

			if (arv == 0) exit(0);
		}
	}

	return 0;
}
