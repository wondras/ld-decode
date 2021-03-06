/************************************************************************

    filter.cpp

    Time-Based Correction
    ld-decode - Software decode of Laserdiscs from raw RF
    Copyright (C) 2018 Chad Page
    Copyright (C) 2018 Simon Inns

    This file is part of ld-decode.

    ld-decode is free software: you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    Email: simon.inns@gmail.com

************************************************************************/

#include "filter.h"

// Public functions
Filter::Filter(int _order, const double *_a, const double *_b)
{
    order = _order + 1;
    if (_a) {
        a.insert(b.begin(), _a, _a + order);
        isIIR = true;
    } else {
        a.push_back(1.0);
        isIIR = false;
    }
    b.insert(b.begin(), _b, _b + order);
    x.resize(order);
    y.resize(order);

    clear(0);
}

Filter::Filter(std::vector<double> _b, std::vector<double> _a)
{
    b = _b;
    a = _a;

    order = b.size();

    x.resize(b.size() + 1);
    y.resize(a.size() + 1);

//			for (int i = 0; i < b.size(); i++) cerr << b[i] << endl;;
//			for (int i = 0; i < b.size(); i++) cerr << a[i] << endl;;

    clear(0);

    isIIR = true;
}

Filter::Filter(Filter *orig)
{
    order = orig->order;
    isIIR = orig->isIIR;
    a = orig->a;
    b = orig->b;
    x.resize(b.size());
    y.resize(a.size());

    clear(0);
}

void Filter::clear(double val)
{
    for (quint16 i = 0; i < a.size(); i++) {
        y[i] = val;
    }
    for (quint16 i = 0; i < b.size(); i++) {
        x[i] = val;
    }
}

void Filter::dump()
{
//			for (int i = 0 ; i < order; i++) cerr << x[i] << ' ' << b[i] << endl;
    qDebug() << (const void *)a.data() << ' ' << a[0];
}

double Filter::feed(double val)
{
    double a0 = a[0];
    double y0;

    double *x_data = x.data();
    double *y_data = y.data();

    memmove(&x_data[1], x_data, sizeof(double) * (b.size() - 1));
    if (isIIR) memmove(&y_data[1], y_data, sizeof(double) * (a.size() - 1));

    x[0] = val;
    y0 = 0; // ((b[0] / a0) * x[0]);
    //cerr << "0 " << x[0] << ' ' << b[0] << ' ' << (b[0] * x[0]) << ' ' << y[0] << endl;
    if (isIIR) {
        for (quint16 o = 0; o < b.size(); o++) {
            y0 += ((b[o] / a0) * x[o]);
        }
        for (quint16 o = 1; o < a.size(); o++) {
            y0 -= ((a[o] / a0) * y[o]);
        }
//				for (int i = 0 ; i < b.size(); i++) cerr << 'b' << i << ' ' << b[i] << ' ' << x[i] << endl;
//				for (int i = 0 ; i < a.size(); i++) cerr << 'a' << i << ' ' << a[i] << ' ' << y[i] << endl;
    } else {
        if (order == 13) {
            double t[4];

            // Cycling through destinations reduces pipeline stalls.
            t[0] = b[0] * x[0];
            t[1] = b[1] * x[1];
            t[2] = b[2] * x[2];
            t[3] = b[3] * x[3];
            t[0] += b[4] * x[4];
            t[1] += b[5] * x[5];
            t[2] += b[6] * x[6];
            t[3] += b[7] * x[7];
            t[0] += b[8] * x[8];
            t[1] += b[9] * x[9];
            t[2] += b[10] * x[10];
            t[3] += b[11] * x[11];
            y0 = t[0] + t[1] + t[2] + t[3] + (b[12] * x[12]);
        } else for (int o = 0; o < order; o++) {
            y0 += b[o] * x[o];
        }
    }

    y[0] = y0;
    return y[0];
}

double Filter::filterValue()
{
    return y[0];
}
