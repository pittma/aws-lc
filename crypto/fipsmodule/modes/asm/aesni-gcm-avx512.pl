if ($#ARGV < 1) { die "Not enough arguments provided.
  Two arguments are necessary: the flavour and the output file path."; }

$flavour = shift;
$output  = shift;

$win64 = 0;
$win64 = 1 if ($flavour =~ /[nm]asm|mingw64/ || $output =~ /\.asm$/);

$avx512vaes = 1;
for (@ARGV) { $avx512vaes = 0 if
   (/-DMY_ASSEMBLER_IS_TOO_OLD_FOR_512AVX/); }

$0 =~ m/(.*[\/\\])[^\/\\]+$/;
$dir = $1;
($xlate = "${dir}x86_64-xlate.pl" and -f $xlate)
  or ($xlate = "${dir}../../../perlasm/x86_64-xlate.pl" and -f $xlate)
  or die "can't locate x86_64-xlate.pl";

open OUT, "| \"$^X\" \"$xlate\" $flavour \"$output\""
  or die "can't call $xlate: $!";
*STDOUT = *OUT;

if ($avx512vaes > 0) {
  my $arg1 = "%rdi";
  my $arg2 = "%rsi";

  $code /= <<___;
  .globl gcm_init_avx512
  .type gcm_init_avx512,\@function,2
  .align 32
  gcm_init_avx512:
    .cfi_startproc
    endbranch
___
} else {
  $code .= <<___;
  .text

  .globl gcm_init_avx512
  .globl gcm_ghash_avx512
  .globl gcm_gmult_avx512
  .globl gcm_setiv_avx512
  .globl aes_gcm_encrypt_avx512
  .globl aes_gcm_decrypt_avx512

  .type gcm_init_avx512,\@abi-omnipotent
  gcm_init_avx512:
  gcm_ghash_avx512:
  gcm_gmult_avx512:
  gcm_setiv_avx512:
  aes_gcm_encrypt_avx512:
  aes_gcm_decrypt_avx512:
      .byte   0x0f,0x0b    # ud2
      ret
  .size   gcm_init_avx512, .-gcm_init_avx512
___
}

print $code;
close STDOUT or die "error closing STDOUT: $!";
