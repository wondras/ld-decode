#include <stdio.h>
#include <iostream>
#include <iomanip>
#include <unistd.h>
#include <sys/fcntl.h>
#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

//#define CHZ 35795453.0 
//#define CHZ (28636363.0*5.0/4.0)

const double FSC=(1000000.0*(315.0/88.0))*1.00;
const double CHZ=(1000000.0*(315.0/88.0))*4.0;

#define LOW 0 

using namespace std;

double _ctor(fftw_complex c)
{
	return sqrt((c[0] * c[0]) + (c[1] * c[1]));
}

double ctor(double r, double i)
{
	return sqrt((r * r) + (i * i));
}

double clamp(double v, double low, double high)
{
	if (v < low) return low;
	else if (v > high) return high;	
	else return v;
}

class LowPass {
	protected:
		bool first;
	public:
		double alpha;
		double val;
		
		LowPass(double _alpha = 0.15) {
			alpha = _alpha;	
			first = true;
		}	

		double feed(double _val) {
			if (first) {
				first = false;
				val = _val;
			} else {
				val = (alpha * val) + ((1 - alpha) * _val);	
			}
			return val;
		}
};

unsigned short rdata[1024*1024*32];
double data[1024*1024*32];
int dlen;

#define STATE_LINE 0
#define STATE_SYNC 1
#define STATE_PORCH 2
#define STATE_CB   3
#define STATE_PORCH2 4
#define STATE_PORCH3 5

int state = STATE_LINE;

int find_sync(int start, int &begin, int &len)
{
	int i, sc = 0;
	begin = len = 0;

	for (i = start; i < dlen; i++) {
		if (!begin) {
			if (data[i] < -20.0) {
				sc++;
				if (sc > 32) {
					begin = i - 32;
				}
			}
		} else if (data[i] > -15.0) {
			len = sc;

			return 0;
		} else sc++;
	}

	return -1;
}

double phase = 0.0;

int cb_analysis(int begin, int end, double &peaklevel, double &peakphase)
{
//	double fc = 0.0, fci = 0.0;
	double freq = 4.0;

	// peaklevel = 0.0;

	for (int i = begin + 16; i < end; i++) {	
		double fc = 0.0, fci = 0.0;
		for (int j = -16; j < 16; j++) {
			double o = (double)(data[i + j]); 

			fc += (o * cos(phase + (2.0 * M_PIl * ((double)(i + j) / freq)))); 
			fci -= (o * sin(phase + (2.0 * M_PIl * ((double)(i + j) / freq)))); 
		}
		double level = ctor(fc, fci) / 33.0;
		if (level > 0.6) phase -= (atan2(fci, ctor(fc, fci)));
		if (level > peaklevel) peaklevel = level;
		cerr << i << ' ' << ctor(fc, fci) / 33 << ' ' << phase << ' ' << peaklevel << endl;
	}
//	cerr << i << ' ' << state << ' ' << (int)data[i] << ':' << ire << ' ' << ' ' << fc << ',' << fci << " : " << ctor(fc, fci) / N << ',' << atan2(fci, ctor(fci, fc)) << ',' << phase << endl; 
//		if (fc < 0) phase += (M_PIl / 2.0); 
//		if (ctor(fc, fci)) phase += (atan2(fci, ctor(fc, fci)));


	//peakfreq = freq;
	peakphase = phase;

	return 0;
}

int main(int argc, char *argv[])
{
	int i, rv, fd;
	unsigned short avg, rhigh = 0, rlow = 65535;
	unsigned short high = 0, low = 65535;
	long long total = 0;
	int rp;
	double igrad = 0.0;

	fd = open(argv[1], O_RDONLY);
	if (argc >= 3) lseek64(fd, atoll(argv[2]) /* *1024*1024 */, SEEK_SET);

	dlen = sizeof(rdata);
	if (argc >= 4) dlen = atol(argv[3]);
	dlen = read(fd, rdata, dlen);
	dlen /= 2.0;

//	cout << std::setprecision(8);

	rlow = 3500;  rhigh = 65535;
	
	igrad = (double)(rhigh - rlow) / 140.0;
	double irestep = 140.0 / (double)(rhigh - rlow); 

	for (i = 0; i < dlen; i++) {
//		data[i] = (sin(2 * M_PIl * ((double)i / (CHZ / FSC))) * 10) + 128; 
//		cout << (sin(2 * M_PIl * ((double)i / 4.0)) * 127) + 128 << endl;

//		data[i] = ((rdata[i] - 25) * 956.0 / 223.0) + 16; 

		data[i] = (((double)rdata[i] - rlow) * irestep)  - 40;
//		cerr << (int)rdata[i] << ' ' << data[i] << endl;

		if (data[i] > high) high = data[i];
		if (data[i] < low)  low = data[i];
		total += data[i];
	}

	rhigh = high;
#if 1
	int begin = 0, len = 0;
	i = 0;
	double burst = 0.0;

	LowPass lpburst(0.8);
	LowPass lpU(0.9), lpV(0.9);

	while (i < dlen) {
		if (!find_sync(i, begin, len)) {
			int lc = 0;
			unsigned char line[1536 * 3];

			cerr << begin << ' ' << len << endl;
			i = begin + len;

			double freq, phase;

			burst = 0.0;
			// color burst is approx i + 30 to i + 90
			cb_analysis(i + 15, i + 35, burst, phase);
			lpburst.feed(burst);

			cerr << freq << ',' << phase << endl;
			freq = 4.0;

			for (int j = i + 60; j < i + 60 + 768; j++) {
				double fc = 0, fci = 0;
				double y = data[j];
		
				for (int k = -7; k < 8; k++) {
					double o = data[j + k]; 

					fc += (o * cos(phase + (2.0 * M_PIl * ((double)(j + k) / freq)))); 
					fci -= (o * sin(phase + (2.0 * M_PIl * ((double)(j + k) / freq)))); 
				}

				double u = (fc / 15 * (16 / lpburst.val));
				double v = (fci / 15 * (16 / lpburst.val));

				lpU.feed(u);
				lpV.feed(v);
				//cerr << lpU.val << ' ' << lpV.val << endl;
#if 0
				if (/*(j >= 6430) && (j <= 6446) && */(burst > 0.2)) {
			//		cerr << j << ' ' << fc << ' ' << fci << ' ' << y << ' ';
//					//cerr << j << ' ' << lpU.val << ' ' << lpV.val << ' ' << y << ' ';
//					y -= (fc ) * cos(phase + (2.0 * M_PIl * (((double)j / freq))));
//					y += (fci ) * sin(phase + (2.0 * M_PIl * (((double)j / freq))));
//					cerr << y << ' ' << endl;
				}
//				y -= (255 * .2);
#endif
				u = lpU.val;
				v = lpV.val;

				y *= 2.55;
				u *= 2.55;
				v *= 2.55;
				clamp(y, 0, 130);
				clamp(u, -78, 78);
				clamp(v, -78, 78);

#if 0
				if (burst > 0.2) {
					y += ((v / burst) * sin(phase + (2.0 * M_PIl * (((double)j / freq)))));
					y -= ((u / burst) * cos(phase + (2.0 * M_PIl * (((double)j / freq)))));
				}
//				y -= (255 * 0.2);
#endif
//			u = v = 0;

/*
B = 1.164(Y - 16)                   + 2.018(U - 128)
G = 1.164(Y - 16) - 0.813(V - 128) - 0.391(U - 128)
R = 1.164(Y - 16) + 1.596(V - 128)
*/

				double r = (y * 1.164) + (1.596 * v); 
				double g = (y * 1.164) - (0.813 * v) - (u * 0.391); 
				double b = (y * 1.164) + (u * 2.018); 

				//cerr << fc << ':' << fci << ' ' << r << endl;				

//				line[lc++] = clamp(y, 0, 255);
	
				line[lc++] = clamp(r, 0, 255);
				line[lc++] = clamp(g, 0, 255);
				line[lc++] = clamp(b, 0, 255);
				//cerr << fc << ':' << fci << ' ' << lc << ' ' << (int)line[lc - 2] << endl;				
			
			}
			write(1, line, 768 * 3);
		} else {
			i = dlen;
		}
	};

/*
	NTSC timing, 4fsc:

	0-767:  data
	795-849: hsync
	795-815: equalizing pulse
	
	340-360, 795-260, 340-715: vsync

	8fsc - 

	0-1535: data
	1590-1699: hsync
	1700-1820: ?

	chroma after conversion:
	sync tip = 16
	chroma peak = 104
	chroma burst = 128
	blank (non sync) 0ire = 240
	black 7.5ire = 280
	peak burst = 352
	white = 800
	peak chroma = 972 

*/

#else

	// cout << dlen << ' ' << (int)high << ' ' << (int)low << ' ' << igrad << endl;

	
//	double irestep = 140.0 / (double)(rhigh - rlow); 

	double freq = (CHZ / FSC);
	double phase = 0.0;
	int sc = 0;


	int lc = 0;
	unsigned char line[2048 * 3];

	for (int i = 16; i < dlen; i++) {
		double fc = 0, fci = 0;
		double ire = ((double)(data[i] - rlow) / igrad)  - 40;

	#define N 16
		for (int j = 0; j < N; j++) {
			double o = (double)(data[i - j]) / igrad; 

			fc += (o * cos(phase + (2.0 * M_PIl * ((double)(i - j) / freq)))); 
			fci -= (o * sin(phase + (2.0 * M_PIl * ((double)(i - j) / freq)))); 
		}
	//	cerr << i << ' ' << state << ' ' << (int)data[i] << ':' << ire << ' ' << ' ' << fc << ',' << fci << " : " << ctor(fc, fci) / N << ',' << atan2(fci, ctor(fci, fc)) << ',' << phase << endl; 
//		if (fc < 0) phase += (M_PIl / 2.0); 
//		if (ctor(fc, fci)) phase += (atan2(fci, ctor(fc, fci)));

	static int first = 1;

		switch (state) {
			case STATE_LINE:
				if (ire < -10.0) {
					sc++;
					if (sc > 16) {
						cerr << lc << endl;
						write(1, line, (2048 * 3));
						memset(line, 0, sizeof(line));
						lc = 0;
						state = STATE_SYNC;
						sc = 0;
					}
				} else {
					if (lc < (2048 * 3)) {
						double y = ire * 2.55;
		
						y = clamp(y, 0, 255);
	
						double u = ((fc / N) * 8);
						double v = ((fci / N) * 8);

/*
B = 1.164(Y - 16)                   + 2.018(U - 128)

G = 1.164(Y - 16) - 0.813(V - 128) - 0.391(U - 128)

R = 1.164(Y - 16) + 1.596(V - 128)
*/

						double r = (y * 1.164) + (1.596 * v); 
						double g = (y * 1.164) - (0.813 * v) - (u * 0.391); 
						double b = (y * 1.164) + (u * 2.018); 
					
						line[lc++] = clamp(r, 0, 255);
						line[lc++] = clamp(g, 0, 255);
						line[lc++] = clamp(b, 0, 255);
	
					}
					sc = 0;
				}	
				break;
			case STATE_SYNC:
				if (ire > -10) state = STATE_PORCH;
				break;
			case STATE_PORCH:
				sc++;
				if ((ctor(fc, fci) / N) > 4.25) state = STATE_CB; 
				break;
			case STATE_CB:
				if ((ctor(fc, fci) / N) < 3.5) {
					first = 0;
					state = STATE_PORCH2;	
				} else if (ctor(fc, fci)) {
					phase -= (atan2(fci, ctor(fc, fci)));
				}
				break;
			case STATE_PORCH2:
				if ((ctor(fc, fci) / N) < 1) state = STATE_PORCH3; 
				break;
			case STATE_PORCH3:
				if ((ire > 5)) state = STATE_LINE; 
				break;
		
		};

	}
#endif	

	return 0;
}
 