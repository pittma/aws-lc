#! /usr/bin/env perl
# Copyright (C) 2025 Intel Corporation

if ($#ARGV < 1) { die "Not enough arguments provided.
  Two arguments are necessary: the flavour and the output file path."; }

$flavour = shift;
$output  = shift;

$win64=0; $win64=1 if ($flavour =~ /[nm]asm|mingw64/ || $output =~ /\.asm$/);

$avx512md5 = 1;
for (@ARGV) { $avx512md5 = 0 if (/-DMY_ASSEMBLER_IS_TOO_OLD_FOR_512AVX/); }

$0 =~ m/(.*[\/\\])[^\/\\]+$/; $dir=$1;
( $xlate="${dir}x86_64-xlate.pl" and -f $xlate ) or
( $xlate="${dir}../../../perlasm/x86_64-xlate.pl" and -f $xlate) or
die "can't locate x86_64-xlate.pl";

open OUT,"| \"$^X\" \"$xlate\" $flavour \"$output\"";
*STDOUT=*OUT;

#======================================================================

if ($avx512md5) {

  my $data  = "%rdi";
  my $length = "%rsi";
  my $out = "%rdx";

  my $completeBlocks = "%r8";

  my $A = 0x67452301;
  my $B = 0xefcdab89;
  my $C = 0x98badcfe;
  my $D = 0x10325476;

  my $F = 0xca;
  my $G = 0xe4;
  my $H = 0x96;
  my $I = 0x39;

  # a = b + ((a + F(b,c,d) + X[k] + T[i]) <<< s).
  sub round_one {
    my $a = _$_[0];
    my $b = _$_[1];
    my $c = _$_[2];
    my $d = _$_[3];
    my $k = _$_[4];
    my $s = _$_[5];
    my $i = _$_[6];

    $code .= <<___;
    vpternlogq $F,$b,$c,$d
    add $a,$d
    add $k,$d
    add $i,$d
    rol $s,$d
    add $b,$d
___
  }

  # uint8_t *md5_x86_64_avx512(const uint8_t *data, size_t len, uint8_t out[MD5_DIGEST_LENGTH])
  $code .= <<___;
    .globl	md5_x86_64_avx512
    .hidden	md5_x86_64_avx512
    .type	md5_x86_64_avx512,\@function,3
    .align	32
    md5_x86_64_avx512:
    .cfi_startproc
    endbranch
    mov $length,$completeBlocks
    shr \$3,$completeBlocks

    .L_main_loop:
    
___
}
