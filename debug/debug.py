#!/usr/bin/env python3

import PSX,sys,time
from array import array

class Color:
    
    @staticmethod
    def get ( r, g, b ):
        return (r<<16)|(g<<8)|b
    
    @staticmethod
    def get_components ( color ):
        return (color>>16,(color>>8)&0xff,color&0xff)
    
class Img:
    
    WHITE= Color.get ( 255, 255, 255 )
    
    def __init__ ( self, width, height ):
        self._width= width
        self._height= height
        self._v= []
        for i in range(0,height):
            self._v.append ( array('i',[Img.WHITE]*width) )
    
    def __getitem__ ( self, ind ):
        return self._v[ind]
    
    def write ( self, to ):
        if type(to) == str :
            to= open ( to, 'wt' )
        to.write ( 'P3\n ')
        to.write ( '%d %d\n'%(self._width,self._height) )
        to.write ( '255\n' )
        for r in self._v:
            for c in r:
                aux= Color.get_components ( c )
                to.write ( '%d %d %d\n'%(aux[0],aux[1],aux[2]) )

def get_tex_page_4bit(fb,page_x,page_y,clut_x,clut_y):
    def get_color(color):
        factor= 255.0/31.0
        r= round((color&0x1f)*factor)
        g= round(((color>>5)&0x1f)*factor)
        b= round(((color>>10)&0x1f)*factor)
        return Color.get(r,g,b)
    off= page_x*64*2 + page_y*256*2048
    off_clut= clut_x*16*2 + clut_y*2048
    ret= Img(256,256)
    for r in range(256):
        off_r= off
        for c in range(128):
            v= fb[off_r];
            off_r+= 1
            ind= off_clut + (v&0xF)*2
            col1= fb[ind] | (fb[ind+1]<<8)
            ind= off_clut + ((v>>4)&0xF)*2
            col2= fb[ind] | (fb[ind+1]<<8)
            ret[r][2*c]= get_color(col1)
            ret[r][2*c+1]= get_color(col2)
            #col1= int((v&0xF)/15.0*255.0)
            #col2= int(((v>>4)&0xF)/15.0*255.0)
            #ret[r][2*c]= Color.get(col1,col1,col1)
            #ret[r][2*c+1]= Color.get(col2,col2,col2)
        off+= 2048
    return ret

def get_tex_page_8bit(fb,page_x,page_y,clut_x,clut_y):
    def get_color(color):
        factor= 255.0/31.0
        r= round((color&0x1f)*factor)
        g= round(((color>>5)&0x1f)*factor)
        b= round(((color>>10)&0x1f)*factor)
        return Color.get(r,g,b)
    off= page_x*64*2 + page_y*256*2048
    off_clut= clut_x*16*2 + clut_y*2048
    ret= Img(256,256)
    for r in range(256):
        off_r= off
        for c in range(256):
            v= fb[off_r]
            off_r+= 1
            ind= off_clut + v*2
            col= fb[ind] | (fb[ind+1]<<8)
            ret[r][c]= get_color(col)
        off+= 2048
    return ret

def get_tex_page_15bit(fb,page_x,page_y):
    def get_color(color):
        factor= 255.0/31.0
        r= round((color&0x1f)*factor)
        g= round(((color>>5)&0x1f)*factor)
        b= round(((color>>10)&0x1f)*factor)
        return Color.get(r,g,b)
    off= page_x*64*2 + page_y*256*2048
    ret= Img(256,256)
    for r in range(256):
        off_r= off
        for c in range(256):
            v= fb[off_r] | (fb[off_r+1]<<8);
            off_r+= 2
            ret[r][c]= get_color(v)
        off+= 2048
    return ret

def get_fb_as15bit(fb):
    def get_color(color):
        factor= 255.0/31.0
        r= round((color&0x1f)*factor)
        g= round(((color>>5)&0x1f)*factor)
        b= round(((color>>10)&0x1f)*factor)
        return Color.get(r,g,b)
    off= 0
    ret= Img(1024,512)
    for r in range(512):
        off_r= off
        for c in range(1024):
            v= fb[off_r] | (fb[off_r+1]<<8)
            off_r+= 2
            col= int((v&0x7FFF)/32767.0*255.0)
            ret[r][c]= Color.get(col,col,col)
        off+= 2048
    return ret

def get_fb_as4bit(fb):
    off= 0
    ret= Img(1024*4,512)
    for r in range(512):
        off_r= off
        for c in range(1024*2):
            v= fb[off_r];
            off_r+= 1
            col1= int((v&0xF)/15.0*255.0)
            col2= int(((v>>4)&0xF)/15.0*255.0)
            ret[r][2*c]= Color.get(col1,col1,col1)
            ret[r][2*c+1]= Color.get(col2,col2,col2)
        off+= 2048
    return ret

mc1= open('MCD1','rb').read()
mc2= open('MCD2','rb').read()
#mc1=None
PSX.init(open('BIOS','rb').read())
#PSX.plug_mem_cards(mc1,None)
#PSX.plug_mem_cards(mc1,mc1)
#PSX.plug_mem_cards(None,None)
PSX.plug_mem_cards(mc1,mc2)
#PSX.loop()
#time.sleep(10)
PSX.set_disc('CUE')
#PSX.set_disc('')
#PSX.set_disc('')
#PSX.set_disc('')
PSX.loop()
#PSX.config_debug(PSX.DBG_GPU_CMD_TRACE)
#PSX.config_debug(PSX.DBG_SHOW_PC_CC|PSX.DBG_GTE_MEM_ACCESS|
#                 PSX.DBG_GTE_CMD_TRACE|PSX.DBG_CPU_INST)
#PSX.config_debug(PSX.DBG_CD_CMD_TRACE|PSX.DBG_CPU_INST)
#PSX.config_debug(PSX.DBG_CD_CMD_TRACE)
#PSX.config_debug(PSX.DBG_SHOW_PC_CC|
#                 PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|PSX.DBG_MEM_ACCESS)
#PSX.config_debug(PSX.DBG_SHOW_PC_CC|PSX.DBG_GTE_MEM_ACCESS|
#                 PSX.DBG_CD_CMD_TRACE|PSX.DBG_DMA_TRANSFER|
#                 PSX.DBG_GPU_CMD_TRACE)
#PSX.config_debug(PSX.DBG_DMA_TRANSFER|PSX.DBG_CD_CMD_TRACE|#PSX.DBG_SHOW_PC_CC|
#                 PSX.DBG_GPU_CMD_TRACE|PSX.DBG_CPU_INST)
#PSX.config_debug(PSX.DBG_CD_CMD_TRACE|PSX.DBG_CPU_INST|
#                 PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|PSX.DBG_MEM_ACCESS)
#PSX.config_debug(PSX.DBG_GPU_CMD_TRACE)
#PSX.config_debug(PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|
#                 PSX.DBG_MEM_ACCESS|
#                 PSX.DBG_CPU_INST|
#                 PSX.DBG_INT_TRACE|
#                 PSX.DBG_CD_CMD_TRACE|
#                 PSX.DBG_GPU_CMD_TRACE|
#                 PSX.DBG_DMA_TRANSFER)
#PSX.config_debug(PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|
#                 PSX.DBG_MEM_ACCESS|PSX.DBG_INT_TRACE|PSX.DBG_SHOW_PC_CC|
#                 PSX.DBG_DMA_TRANSFER)
#PSX.config_debug(PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|
#                 PSX.DBG_MEM_ACCESS)
#PSX.config_debug(PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|
#                 PSX.DBG_MEM_ACCESS|PSX.DBG_SHOW_PC_CC)
#PSX.config_debug(PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|
#                 PSX.DBG_MEM_ACCESS|PSX.DBG_SHOW_PC_CC)
#PSX.config_debug(PSX.DBG_GPU_CMD_TRACE)
#PSX.config_debug(PSX.DBG_INT_TRACE)
#PSX.config_debug(PSX.DBG_INT_TRACE|PSX.DBG_MEM_ACCESS16|
#                 PSX.DBG_MEM_ACCESS8|PSX.DBG_MEM_ACCESS|
#                 PSX.DBG_CD_CMD_TRACE|PSX.DBG_GPU_CMD_TRACE)
#PSX.config_debug(PSX.DBG_GPU_CMD_TRACE|PSX.DBG_INT_TRACE|PSX.DBG_SHOW_PC_CC|
#                 PSX.DBG_DMA_TRANSFER)
#PSX.config_debug(PSX.DBG_GPU_CMD_TRACE|PSX.DBG_SHOW_PC_CC)
#PSX.config_debug(PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|
#                 PSX.DBG_MEM_ACCESS)
PSX.config_debug(PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|
                 PSX.DBG_MEM_ACCESS|PSX.DBG_SHOW_PC_CC|PSX.DBG_INT_TRACE)
#PSX.config_debug(PSX.DBG_CPU_INST)
#PSX.config_debug(PSX.DBG_CPU_INST|PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|
#                 PSX.DBG_MEM_ACCESS|PSX.DBG_SHOW_PC_CC|PSX.DBG_INT_TRACE)
#PSX.config_debug(PSX.DBG_GPU_CMD_TRACE|PSX.DBG_SHOW_PC_CC|
#                 PSX.DBG_DMA_TRANSFER|PSX.DBG_MEM_ACCESS16|PSX.DBG_MEM_ACCESS8|
#                 PSX.DBG_MEM_ACCESS)
#PSX.trace(400000000)
#PSX.trace(200000000)
#PSX.trace(100000000)
#PSX.trace(60000000)
#PSX.trace(50000000)
#PSX.trace(30000000)
#PSX.trace(20000000)
#PSX.trace(10000000)
#fb= PSX.get_frame_buffer()
#get_tex_page_8bit(fb,9,0,4,511).write('texture1.pnm')
#get_tex_page_8bit(fb,7,0,4,511).write('texture2.pnm')
#get_tex_page_8bit(fb,5,0,4,511).write('texture3.pnm')
#get_tex_page_4bit(fb,12,0,51,255).write('texture4.pnm')
#get_fb_as4bit(fb).write('fb4bit.pnm')
#get_fb_as15bit(fb).write('fb15bit.pnm')
