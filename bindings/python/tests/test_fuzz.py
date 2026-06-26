"""Coverage-guided fuzzing + mutation testing (Track E) through the emulator
binding, over a hand-assembled classify(x) -> {-1,0,+1} (host-independent)."""
import asmtest

# xor eax,eax ; test rdi ; js neg ; test rdi ; jz zero ; mov eax,1 ; ret ;
# (neg) mov eax,-1 ; ret  — three branch paths.
CLASSIFY3 = bytes([
    0x31, 0xC0, 0x48, 0x85, 0xFF, 0x78, 0x0B, 0x48, 0x85, 0xFF, 0x74, 0x05,
    0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3,
])


def test_fuzz_coverage_beats_fixed_vector():
    with asmtest.Emulator() as e:
        # A single fixed vector (x=5, the positive path) reaches a few blocks.
        fixed = e.fuzz_cover(CLASSIFY3, 5, 5, 1)
        guided = e.fuzz_cover(CLASSIFY3, -50, 50, 2000)
        assert guided.blocks_reached > fixed.blocks_reached  # found the neg path
        assert guided.corpus_len >= 2


def test_mutation_strong_suite_kills_more():
    with asmtest.Emulator() as e:
        weak = e.mutation_test(CLASSIFY3, [5])            # positive path only
        strong = e.mutation_test(CLASSIFY3, [-7, 0, 9])   # all three paths
        assert weak.mutants == strong.mutants
        assert weak.survived > 0                          # weak suite misses some
        assert strong.survived < weak.survived            # strong kills more
        assert strong.killed > weak.killed
