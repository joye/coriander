// Copyright Hugh Perkins 2016

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cocl/cocl_dnn.h"

#include "cocl/cocl.h"
#include "EasyCL/EasyCL.h"

#include <iostream>
#include <memory>
#include <sstream>

#include "gtest/gtest.h"

using namespace std;
using namespace cocl;
using namespace easycl;

namespace {

typedef std::mt19937 MT19937;

void im2col_cpu(float *imageStack, int C, int inH, int inW, int kH, int kW, int padH, int padW, int dH, int dW, float *col) {
    int outH = (inH + 2 * padH - kH) / dH + 1;
    int outW = (inW + 2 * padW - kW) / dW + 1;
    int colRows = C * kH * kW;
    int colCols = outH * outW;
    // zero first
    int colLinearSize = colRows * colCols;
    for(int i = 0; i < colLinearSize; i++) {
        col[i] = 0.0f;
    }
    for(int outh=0; outh < outH; outh++) {
        for(int outw=0; outw < outW; outw++) {
            for(int c=0; c < C; c++) {
                for(int kh=0; kh < kH; kh++) {
                    int inh = outh + kh - padH;
                    for(int kw=0; kw < kW; kw++) {
                        int inw = outw + kw - padW;
                        if(inh >= 0 && inh < inH && inw >= 0 && inw < inW) {
                            float val = imageStack[c*inH*inW + inh * inW + inw];
                            col[(c * kH * kW + kh * kW + kw) * colCols +
                                outh * outW + outw] = val;
                        }
                    }
                }
            }
        }
    }
}

void fillRandomUniform(MT19937 &random, float *target, int size, float minVal, float maxVal) {
    for(int i = 0; i < size; i++) {
        target[i] = random() / (float)random.max();
    }
}

TEST(test_dnn, simple_cpu_im2col) {
    int C = 3;
    int inH = 5;
    int inW = 5;
    int kH = 3;
    int kW = 3;
    int padH = 1;
    int padW = 1;
    int dH = 1;
    int dW = 1;

    int outH = (inH + 2 * padH - kH) / dH + 1;
    int outW = (inW + 2 * padW - kW) / dW + 1;

    float *inImageStack = new float[C * inH * inW];
    int colRows = C * kH * kW;
    int colCols = outH * outW;
    float *outCol = new float[colRows * colCols];

    MT19937 random;
    random.seed(123ul);

    fillRandomUniform(random, inImageStack, C * inH * inW, 0.0f, 1.0f);
    im2col_cpu(inImageStack, C, inH, inW, kH, kW, padH, padW, dH, dW, outCol);

    cout << "input image stack:" << endl;
    for(int c=0; c < C; c++) {
        cout << "C=" << c << endl;
        for(int inh=0; inh < inH; inh++) {
            ostringstream oss;
            for(int inw=0; inw < inW; inw++) {
                oss << inImageStack[c * inH * inW + inh * inW + inw] << " ";
            }
            cout << oss.str() << endl;
        }
        cout << endl;
    }
    cout << endl;

    im2col_cpu(inImageStack, C, inH, inW, kH, kW, padH, padW, dH, dW, outCol);
// int C, int inH, int inW, int kH, int kW, int padH, int padW, int dH, int dW, float *col

    cout << "output cols" << endl;
    for(int colRow = 0; colRow < colRows; colRow++) {
        ostringstream oss;
        for(int colCol=0; colCol < colCols; colCol++) {
            oss << outCol[colRow * colCols + colCol] << " ";
        }
        cout << oss.str() << endl;
    }

    // test_cpu_im2col(3, 5, 5, 3, 3);
    // ThreadVars *v = getThreadVars();
    // EasyCL *cl = v->getContext()->getCl();

    // const int N = 1024;
    // Memory *memory = Memory::newDeviceAlloc(N * sizeof(float));
    // kernel1->inout(&memory->clmem);
    // kernel1->run_1d(32, 32);
    // float *hostdata = new float[N];
    // cl_int err;
    // err = clEnqueueReadBuffer(v->currentContext->default_stream.get()->clqueue->queue, memory->clmem, CL_TRUE, 0,
    //                                  N * sizeof(float), hostdata, 0, NULL, NULL);
    // EasyCL::checkError(err);
    // cout << "hostdata[0] " << hostdata[0] << endl;
    // EXPECT_EQ(123.0f, hostdata[0]);

    // delete [] hostdata;

    delete[] outCol;
    delete[] inImageStack;
}

} // namespace
