/*======================================================================*/
/* Proxy kernel linker script - with embedded FAT image for simulation  */
/*======================================================================*/

OUTPUT_ARCH( "riscv" )
ENTRY(_start)

MEMORY
{
  L2      (rwx) : ORIGIN = 0x1C000000, LENGTH = 0x00008000
  L3      (rwx) : ORIGIN = 0x80000000, LENGTH = 0x01C00000
  FATIMG  (r)   : ORIGIN = 0x81C00000, LENGTH = 0x00200000
}

SECTIONS
{
  . = 0x80000000;
  .text.init : { *(.text.init) } > L3
  . = 0x80000100;
  .nocache_share_region : { *(.nocache_share_region) } > L3
  . = ALIGN(0x1000);
  .tohost : { *(.tohost) } > L2

  .text : { *(.text) } > L3

  /* SPM DATA */
  .spm_data : { *(.heapl2ram) } > L2

  .data : { *(.data) } > L3

  .sdata : {
    __global_pointer$ = . + 0x800;
    *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2) *(.srodata*)
    *(.sdata .sdata.* .gnu.linkonce.s.*)
  } > L3

  .rodata :  {
    *(.rodata);
    . = ALIGN(0x10);
    __cluster_code_start = . ;
    KEEP(*(.clusterraw*))
    __cluster_code_end = . ;
    } > L3

  .sbss : {
    *(.sbss .sbss.* .gnu.linkonce.sb.*)
    *(.scommon)
  } > L3
  .bss : {
    *(.bss)
  } > L3

  .tdata :
  {
    _tls_data = .;
    *(.tdata.begin)
    *(.tdata)
    *(.tdata.end)
  } > L3
  .tbss :
  {
    *(.tbss)
    *(.tbss.end)
  } > L3

  .stack :
  {
    . = . + 0x8000;
    _stack = .;
  } > L3

  _end = .;
  .vectors :
    {
        . = ALIGN(4);
        _vectors_start = .;
        KEEP(*(.vectors))
        _vectors_end = .;
    } > L3

  /* FAT image embedded at fixed address */
  .fat_image : {
    *(.fat_image)
  } > FATIMG
}
