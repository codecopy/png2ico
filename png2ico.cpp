/* Copyright (C) 2002 Matthias S. Benkmann <m.s.b@gmx.net>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2
of the License (ONLY THIS VERSION).

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

/*
Notes about transparent and inverted pixels:
 Handling of transparent pixels is inconsistent in Windows. Sometimes a
 pixel with an AND mask value of 1 is just transparent (i.e. its color 
 value is ignored), sometimes the color value is XORed with the background to
 give some kind of inverted effect. A closer look at bmp.txt suggests that
 the latter behaviour is the correct one but because it often doesn't happen
 it's de facto undefined behaviour.
 Furthermore, sometimes the AND mask entry seems to be interpreted as a
 color index, i.e. a value of 1 will AND the background with color 1.
 Conclusion: The most robust solution seems to be:
               -color 0 always 0,0,0
               -color 1 always 255,255,255
               -all transparent pixels get color 0
*/


#include <cstdio>
#include <vector>

#if __GNUC__ > 2
#include <ext/hash_map>
#else
#include <hash_map>
#endif

#include <png.h>

#include "VERSION"

using namespace std;
namespace __gnu_cxx{};
using namespace __gnu_cxx;

const int word_max=65535;
const int transparency_threshold=196;

void writeWord(FILE* f, int word)
{
  char data[2];
  data[0]=(word&255);
  data[1]=(word>>8)&255;
  if (fwrite(data,2,1,f)!=1) {perror("Write error"); exit(1);};
};

void writeDWord(FILE* f, unsigned int dword)
{
  char data[4];
  data[0]=(dword&255);
  data[1]=(dword>>8)&255;
  data[2]=(dword>>16)&255;
  data[3]=(dword>>24)&255;
  if (fwrite(data,4,1,f)!=1) {perror("Write error"); exit(1);};
};

void writeByte(FILE* f, int byte)
{
  char data[1];
  data[0]=(byte&255);
  if (fwrite(data,1,1,f)!=1) {perror("Write error"); exit(1);};
};

int andMaskLineLen(int width)
{
  int len=(width+7)>>3;
  return (len+3)&~3;
};

int xorMaskLineLen(int width)
{
  return (width+3)&~3;
};

struct png_data
{
  png_structp png_ptr;
  png_infop info_ptr;
  png_infop end_info;
  png_uint_32 width, height;
  png_colorp palette;
  png_bytepp transMap;
  int num_palette;
  png_data():png_ptr(NULL),info_ptr(NULL),end_info(NULL),width(0),height(0),
             palette(NULL),transMap(NULL),num_palette(0){};
};

typedef bool (*checkTransparent_t)(png_bytep, png_data&);

bool checkTransparent1(png_bytep data, png_data&)
{
  return (data[3]<transparency_threshold);
};

bool checkTransparent3(png_bytep, png_data&)
{
  return false;
};

//returns true if input had too many colors
bool convertToIndexed(png_data& img, bool hasAlpha)
{
  img.palette=(png_colorp)malloc(sizeof(png_color)*256);
  img.num_palette=0;
  
  checkTransparent_t checkTrans=checkTransparent1;
  int bytesPerPixel=4;
  if (!hasAlpha)
  {
    bytesPerPixel=3;
    checkTrans=checkTransparent3;  
  };
  
  //first pass: gather all colors, make sure
  //alpha channel (if present) contains only 0 and 255
  //if an alpha channel is present, set all transparent pixels to RGBA (0,0,0,0)
  //transparent pixels will already be mapped to palette entry 0, non-transparent
  //pixels will not get a mapping yet (-1)
  hash_map<unsigned int,signed int> mapQuadToPalEntry;
  png_bytep* row_pointers=png_get_rows(img.png_ptr, img.info_ptr);
  
  for (int y=img.height-1; y>=0; --y)
  {
    png_bytep pixel=row_pointers[y];
    for (unsigned i=0; i<img.width; ++i)
    {
      unsigned int quad=pixel[0]+(pixel[1]<<8)+(pixel[2]<<16);
      bool trans=(*checkTrans)(pixel,img);
     
      if (hasAlpha) 
      {
        if (trans)
        {
          pixel[0]=0;
          pixel[1]=0;
          pixel[2]=0;
          pixel[3]=0;
          quad=0;
        }
        else pixel[3]=255;
        
        quad+=(pixel[3]<<24);
      }
      else if (!trans) quad+=(255<<24);
      
      if (trans) 
        mapQuadToPalEntry[quad]=0;
      else  
        mapQuadToPalEntry[quad]=-1;
    
      pixel+=bytesPerPixel;
    };  
  };
  
  //always allocate entry 0 to black and entry 1 to white because
  //sometimes AND mask is interpreted as color index
  img.num_palette=2;
  img.palette[0].red=0;
  img.palette[0].green=0;
  img.palette[0].blue=0;
  img.palette[1].red=255;
  img.palette[1].green=255;
  img.palette[1].blue=255;
  
  mapQuadToPalEntry[255<<24]=0; //map (non-transparent) black to entry 0
  mapQuadToPalEntry[255+(255<<8)+(255<<16)+(255<<24)]=1; //map (non-transparent) white to entry 1

  int transLineLen=andMaskLineLen(img.width);
  int transLinePad=transLineLen - ((img.width+7)/8);
  img.transMap=(png_bytepp)malloc(img.height*sizeof(png_bytep));

  bool tooManyColors=false;
  
  //second pass: convert RGB to palette entries
  //no actual color reduction is performed. Palette entries are assigned on a
  //"first come, first served" basis. Once all entries are assigned, new colors
  //get palette entry 0
  //NOTE that transparent pixels have already been mapped to entry 0
  //entry 0 is always (0,0,0) 
  //and entry 1 is always (255,255,255)
  for (int y=img.height-1; y>=0; --y)
  {
    png_bytep row=row_pointers[y];
    png_bytep pixel=row;
    int count8=0;
    int transbyte=0;
    png_bytep transPtr=img.transMap[y]=(png_bytep)malloc(transLineLen);
    
    for (unsigned i=0; i<img.width; ++i)
    {
      bool trans=((*checkTrans)(pixel,img));
      unsigned int quad=pixel[0]+(pixel[1]<<8)+(pixel[2]<<16);
      if (hasAlpha) quad+=(pixel[3]<<24); else if (!trans) quad+=(255<<24);
      
      if (trans) ++transbyte; 
      if (++count8==8)
      {
        *transPtr++ = transbyte;
        count8=0;
        transbyte=0;
      };
      transbyte+=transbyte; //shift left 1
      
      int palentry=mapQuadToPalEntry[quad];
      if (palentry<0)
      {
        if (img.num_palette<256)
        {
          palentry=img.num_palette;
          img.palette[palentry].red=pixel[0];
          img.palette[palentry].green=pixel[1];
          img.palette[palentry].blue=pixel[2];
          ++img.num_palette;
        }
        else {tooManyColors=true; palentry=0;};
        mapQuadToPalEntry[quad]=palentry;
      };

      row[i]=palentry;
      pixel+=bytesPerPixel;
    };

    for(int i=0; i<transLinePad; ++i) *transPtr++ = 0;
  };
  
  return tooManyColors;
};



int main(int argc, char* argv[])
{
  if (argc<3)
  {
    fprintf(stderr,version"\n");
    fprintf(stderr,"USAGE: png2ico icofile pngfile1 [pngfile2 ...]\n");
    exit(1);
  };
  
  if (argc-2 > word_max) 
  {
    fprintf(stderr,"Too many PNG files\n");
    exit(1);
  };
  
  vector<png_data> pngdata;
  
  //i is static because used in a setjmp() block
  for (static int i=2; i<argc; ++i)
  {
    FILE* pngfile=fopen(argv[i],"rb");
    if (pngfile==NULL)  {perror(argv[i]); exit(1);};
    png_byte header[8];
    if (fread(header,8,1,pngfile)!=1) {perror(argv[i]); exit(1);};
    if (png_sig_cmp(header,0,8))
    {
      fprintf(stderr,"%s: Not a PNG file\n",argv[i]);
      exit(1);
    };
    
    png_data data;
    data.png_ptr=png_create_read_struct
                   (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!data.png_ptr)
    {
      fprintf(stderr,"png_create_read_struct error\n");
      exit(1);
    };  

    data.info_ptr=png_create_info_struct(data.png_ptr);
    if (!data.info_ptr)
    {
      png_destroy_read_struct(&data.png_ptr, (png_infopp)NULL, (png_infopp)NULL);
      fprintf(stderr,"png_create_info_struct error\n");
      exit(1);
    };

    data.end_info=png_create_info_struct(data.png_ptr);
    if (!data.end_info)
    {
      png_destroy_read_struct(&data.png_ptr, &data.info_ptr, (png_infopp)NULL);
      fprintf(stderr,"png_create_info_struct error\n");
      exit(1);
    };
    
    if (setjmp(png_jmpbuf(data.png_ptr)))
    {
      png_destroy_read_struct(&data.png_ptr, &data.info_ptr, &data.end_info);
      fprintf(stderr,"%s: PNG error\n",argv[i]);
      exit(1);
    };
    
    png_init_io(data.png_ptr, pngfile);
    png_set_sig_bytes(data.png_ptr,8);
    int trafo=PNG_TRANSFORM_PACKING|PNG_TRANSFORM_STRIP_16|PNG_TRANSFORM_EXPAND;
    png_read_png(data.png_ptr, data.info_ptr, trafo , NULL);

    int bit_depth, color_type, interlace_type, compression_type, filter_method;
    png_get_IHDR(data.png_ptr, data.info_ptr, &data.width, &data.height, &bit_depth, &color_type, 
                 &interlace_type, &compression_type, &filter_method);
                 
    
    
    if ( (data.width&7)!=0 || data.width>=256 || data.height>=256)
    {
      //I don't know if the following is really a requirement (bmp.txt says that
      //only 16x16, 32x32 and 64x64 are allowed but that doesn't seem right) but
      //if the width is not a multiple of 8, then the loop creating the and mask later
      //doesn't work properly because it doesn't shift in padding bits
      fprintf(stderr,"%s: Width must be multiple of 8 and <256. Height must be <256.\n",argv[i]);
      exit(1);
    };
    
    if ((color_type & PNG_COLOR_MASK_COLOR)==0)
    {
      fprintf(stderr,"%s: Grayscale image not supported\n",argv[i]);
      exit(1);
    };
    
    if (color_type==PNG_COLOR_TYPE_PALETTE)
    {
      fprintf(stderr,"This can't happen. PNG_TRANSFORM_EXPAND transforms image to RGB.\n");
      exit(1);
    }
    else
    {
      if (convertToIndexed(data, ((color_type & PNG_COLOR_MASK_ALPHA)!=0)))
      {
        fprintf(stderr,"%s: Too many colors! Excess colors mapped to black!\n",argv[i]);
      };
    };  
    
    pngdata.push_back(data);
    
    fclose(pngfile);
  };
  

  
  FILE* outfile=fopen(argv[1],"wb");
  if (outfile==NULL) {perror(argv[1]); exit(1);};
  
  writeWord(outfile,0); //idReserved
  writeWord(outfile,1); //idType
  writeWord(outfile,pngdata.size());
  
  int offset=6+pngdata.size()*16;
  
  vector<png_data>::const_iterator img;
  for(img=pngdata.begin(); img!=pngdata.end(); ++img)
  {
    writeByte(outfile,img->width); //bWidth
    writeByte(outfile,img->height); //bHeight
    writeByte(outfile,0); //bColorCount (0=>256)
    writeByte(outfile,0); //bReserved
    writeWord(outfile,0); //wPlanes
    writeWord(outfile,0); //wBitCount
    int resSize=40+256*4+(andMaskLineLen(img->width)+xorMaskLineLen(img->width))*img->height;
    writeDWord(outfile,resSize);
    writeDWord(outfile,offset); //dwImageOffset
    offset+=resSize;
  };
  
  
  for(img=pngdata.begin(); img!=pngdata.end(); ++img)
  {
    int xorLinePad=xorMaskLineLen(img->width) - img->width;
    
    writeDWord(outfile,40); //biSize
    writeDWord(outfile,img->width); //biWidth
    writeDWord(outfile,2*img->height); //biHeight
    writeWord(outfile,1);   //biPlanes
    writeWord(outfile,8);   //biBitCount
    writeDWord(outfile,0);  //biCompression
    writeDWord(outfile,(andMaskLineLen(img->width)+xorMaskLineLen(img->width))*img->height);  //biSizeImage
    writeDWord(outfile,0);  //biXPelsPerMeter
    writeDWord(outfile,0);  //biYPelsPerMeter
    writeDWord(outfile,0); //biClrUsed (MUST BE 0 ACCORDING TO bmp.txt!!! I tried putting the real number here, but this breaks icons in some places)
    writeDWord(outfile,0);   //biClrImportant
    for (int i=0; i<256; ++i)
    {
      char col[4];
      memset(col,0,4);
      if (i<img->num_palette)
      {
        col[0]=img->palette[i].blue;
        col[1]=img->palette[i].green;
        col[2]=img->palette[i].red;
      };
      if (fwrite(col,4,1,outfile)!=1) {perror("Write error"); exit(1);};
    };
    
    png_bytep* row_pointers=png_get_rows(img->png_ptr, img->info_ptr);
    for (int y=img->height-1; y>=0; --y)
    {
      png_bytep row=row_pointers[y];
      if (fwrite(row,img->width,1,outfile)!=1) {perror("Write error"); exit(1);};
      for(int i=0; i<xorLinePad; ++i) writeByte(outfile,0);
    };
    
    for (int y=img->height-1; y>=0; --y)
    {
      png_bytep transPtr=img->transMap[y];
      if (fwrite(transPtr,andMaskLineLen(img->width),1,outfile)!=1) {perror("Write error"); exit(1);};
    };
  };
  
  fclose(outfile);
};

