/*
	Fast general purpose data preprocessor
	
	The MIT License (MIT)

	Copyright (c) 2016 Lucas Marsh

	How it works:
		encoding:
			pass 1: skims over input file to get a rough idea of how many channels there are
			pass 2: encodes a 1 byte header with the amount of channels then encodes the entire file
		decoding is a single pass that un-interleaves the data with n channels.
	
		encode method is either delta (best for image) or adaptive LPC with a single weight (best for audio)
*/

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#define blocksize 24576
#define boost 24
#define totalChannels 15
#define breakpoint 10

typedef uint8_t byte;

byte indexToChannel[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 6, 8}; // 0-9 Delta, 10-15 Adaptive

int weight = 0; // weight [-1, 1]
int rate = 6; // learning rate for filter
char buffer[blocksize];

/// delta ///
byte previous[8]; // up to 8 channels for delta

byte deltaEnc(byte b, int i)
{
    byte delta = previous[i] - b;
    previous[i] = b;
    return delta;
}
 
byte deltaDec(byte delta, int i)
{
    byte b = previous[i] - delta;
    previous[i] = b;
    return b;
}

/// adaptive LPC ///
byte previousByte[8]; // sample 1 
byte secondPreviousByte[8]; // sample 2 

void updateWeight(byte err)
{
	if (err < 127) weight++;
	if (err > 127) weight--;
	if (weight == 1281) weight--;
	if (weight == -1281) weight++;
}

byte adaptiveDeltaEnc(byte b, int i)
{
	// find error of prediction
	byte prediction = (previousByte[i] - secondPreviousByte[i]) + previousByte[i];

	int w = (weight >> rate);
	byte error = w + (prediction - b);

	// update variables
	updateWeight(error);
	secondPreviousByte[i] = previousByte[i];
	previousByte[i] = w + b; // store a refined representation of history (slightly improves compression)

	return error;
}

byte adaptiveDeltaDec(byte error, int i)
{
	// find error of prediction
	byte prediction = (previousByte[i] - secondPreviousByte[i]) + previousByte[i];

	int w = (weight >> rate);
	byte b = w + (prediction - error);

	// update variables
	updateWeight(error);
	secondPreviousByte[i] = previousByte[i];
	previousByte[i] = w + b;

	return b;
}

/// synthetic modulo ///
int d = 0;

int modulo(int max)
{
	d++;
	if(d == max) d = 0;
	return d;
}

void resetModulo()
{
	d = 0;
}

/// entropy calc ///
long freq[totalChannels][256];
double fileLength;

void count(byte current, int channel)
{
	freq[channel][current]++;
}

double calcEntropy(int channel)
{
	double entropy = 0;
	for (int i = 0; i < 256; i++)
	{
		double charProbability = freq[channel][i] / fileLength;
		if (charProbability != 0)
		{
			entropy += (charProbability)*(-log(charProbability) / log(2));
		}
	}
	return entropy;
}

/// find best encode method ///
int findSmallestChannel()
{
	double chanEnt[totalChannels];
	for (int i = 0; i < totalChannels; i++)
	{
		chanEnt[i] = calcEntropy(i);
	}
	/// find smallest entropy ///
	int index = 0;
	double min = chanEnt[index];
	for (int i = 1; i < totalChannels; i++)
	{
		if (chanEnt[i] < min)
		{
			min = chanEnt[i];
			index = i;
		}
	}
	return index;
}

/// scan for best encode method //
int scan(FILE* in)
{
	int read = 0;

	/// gather some file info ///
	fseek(in, 0, SEEK_END);
	fileLength = ftell(in);
	fseek(in, 0, SEEK_SET);
	int eof = fileLength;

	/// scan input ///
	while (feof(in) == 0)
	{
		read = fread(&buffer, 1, blocksize, in);

		/// test all encoding methods ///
		for (int index = 0; index < totalChannels; index++)
		{
			int channel = indexToChannel[index];

			for (int i = 0; i < read; i++)
			{
				if (channel == 0)
				{
					count(buffer[i], index);
				}
				else if (index < 7)
				{
					int mod = modulo(channel);
					count(deltaEnc(buffer[i], mod), index);
				}
				else if(index >= 7)
				{
					int mod = modulo(channel);
					count(adaptiveDeltaEnc(buffer[i], mod), index);
				}
			}
			resetModulo();
		}
		// if there's room to stride, stride
		if ((ftell(in) + (blocksize*boost)) < eof)
		{
			fseek(in, (blocksize*boost), SEEK_CUR);
		}

	}

	int channel = findSmallestChannel();

	// reset variables
	for (int j = 0; j < 8; j++)
	{
		previous[j] = 0;
		previousByte[j] = 0;
		secondPreviousByte[j] = 0;
		weight = 0;
	}
	return channel;
}

void encode(int channel, FILE* in, FILE* out)
{	
	int read = 0;
	
	if(channel < breakpoint)
		printf("\nencoding channel %i standard\n", indexToChannel[channel]);
	else
		printf("\nencoding channel %i adaptive\n", indexToChannel[channel]);
	
	/// write header ///
	putc(channel, out);
	rewind(in);
	
	/// encode ///
	int ch = indexToChannel[channel];
	while (feof(in) == 0)
	{
		read = fread(buffer, 1, blocksize, in);
		// encode
		if(channel < breakpoint)
		{
			if (ch != 0)
			{
				for (int i = 0; i < read; i++)
				{
					int mod = modulo(ch);
					buffer[i] = deltaEnc(buffer[i], mod);
				}
			}
		}
		else
		{
			if (ch != 0)
			{
				for (int i = 0; i < read; i++)
				{
					int mod = modulo(ch);
					buffer[i] = adaptiveDeltaEnc(buffer[i], mod);
				}
			}
		}
			fwrite(buffer, 1, read, out);
	}
}

void decode(FILE* in, FILE* out)
{
	int channel = getc(in);
	
	if(channel < breakpoint)
		printf("\ndecoding channel %i standard\n", indexToChannel[channel]);
	else
		printf("\ndecoding channel %i adaptive\n", indexToChannel[channel]);
		
	// set variables //
	for (int j = 0; j < 8; j++)
	{
		previous[j] = 0;
		previousByte[j] = 0;
		secondPreviousByte[j] = 0;
	}
	weight = 0;
	int read = 0;
	
	int ch = indexToChannel[channel];
	while (feof(in) == 0)
	{
		read = fread(buffer, 1, blocksize, in);
		// decode //
		if(channel < breakpoint)
		{
			if (ch != 0)
			{
				for (int i = 0; i < read; i++)
				{
					int mod = modulo(ch);
					buffer[i] = deltaDec(buffer[i], mod);
				}
			}
		}
		else
		{
			if (ch != 0)
			{
				for (int i = 0; i < read; i++)
				{
					int mod = modulo(ch);
					buffer[i] = adaptiveDeltaDec(buffer[i], mod);
				}
			}
		}
		fwrite(buffer, 1, read, out);
	}
}

/// main ///
int main(int argc, char* argv[])
{
	time_t start, end;
	start = clock();

	// Check arguments //
	if(argc != 4)
	{
		printf("usage: prepack_light.exe e/d in out\n");
	        return 1;
	}
	else
	{
	        // Check input and output //
	 	FILE* in, *out;
	        if((in=fopen(argv[2],"rb"))==NULL)
	        {
			printf("no input!\n");
			return 2;
		}
		if((out=fopen(argv[3],"wb"))==NULL)
		{
			printf("no output!\n");
			return 3;
		}
	        
	        if(argv[1][0]=='e')
	        {
	            char method = scan(in);
	            encode(method, in, out);
	        }
	        else if(argv[1][0]=='d')
	        {
	            decode(in, out);
	        }
	        else
	        {
	            printf("unknown argument!\n");
	            return 4;
	        }
		fclose(in);
		fclose(out);
	}

end = clock();
printf("took %li seconds\n", (end - start) / CLOCKS_PER_SEC);
return 0;
}
