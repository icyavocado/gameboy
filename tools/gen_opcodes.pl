#!/usr/bin/perl
use strict;
use warnings;
use JSON::PP qw(decode_json);

# Generate opcode timing metadata from the gbdev JSON table.
my $source = $ARGV[0] // 'tools/Opcodes.json';
my $output = $ARGV[1] // 'src/core/opcodes_generated.c';

open my $in, '<', $source or die "cannot read $source: $!\n";
local $/;
my $data = decode_json(<$in>);
close $in;
my $items = exists $data->{unprefixed} ? $data->{unprefixed} : $data;

open my $out, '>', $output or die "cannot write $output: $!\n";
print {$out} "#include <stdint.h>\n\n";
print {$out} "const uint8_t gb_opcode_cycles[256] = {\n";
for my $i (0 .. 255) {
    my $key = sprintf '0x%02X', $i;
    my $entry = $items->{$key} // $items->{sprintf('0x%02x', $i)} // $items->{$i} // {};
    my $cycles = ref($entry) eq 'HASH' ? ($entry->{cycles} // 4) : 4;
    $cycles = $cycles->[0] if ref($cycles) eq 'ARRAY';
    printf {$out} "  %d,%s", $cycles, ($i % 16 == 15 ? "\n" : '');
}
print {$out} "};\n";
close $out;
