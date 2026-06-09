-- Tags: long
-- Reproduction for issue #105351: with the new analyzer, INSERT and CREATE TABLE
-- on a MATERIALIZED column whose expression contains many repeated subexpressions
-- spent quadratic time in `IQueryTreeNode::getTreeHash`. The test builds a
-- moderately-sized MATERIALIZED expression and repeatedly forces re-analysis so
-- the framework's wall-clock timeout converts a regression back to quadratic
-- behaviour into a hard failure rather than just a slow run. Without the
-- per-node hash memoization fix this query was observed taking tens of seconds
-- and triggered the standard 180s timeout on heavier schemas.

DROP TABLE IF EXISTS t_105351;

-- A MATERIALIZED column with a 100-branch multiIf whose result type is a Tuple,
-- referenced many times in the expression. The repeated subexpressions force the
-- analyzer to re-hash overlapping subtrees on every `resolveFunction` call.
CREATE TABLE t_105351
(
    x UInt32,
    big Tuple(a UInt32, b UInt32, c UInt32) MATERIALIZED multiIf(
        x = 0,  tuple(toUInt32(0),  toUInt32(0),  toUInt32(0)),
        x = 1,  tuple(toUInt32(1),  toUInt32(1),  toUInt32(1)),
        x = 2,  tuple(toUInt32(2),  toUInt32(2),  toUInt32(2)),
        x = 3,  tuple(toUInt32(3),  toUInt32(3),  toUInt32(3)),
        x = 4,  tuple(toUInt32(4),  toUInt32(4),  toUInt32(4)),
        x = 5,  tuple(toUInt32(5),  toUInt32(5),  toUInt32(5)),
        x = 6,  tuple(toUInt32(6),  toUInt32(6),  toUInt32(6)),
        x = 7,  tuple(toUInt32(7),  toUInt32(7),  toUInt32(7)),
        x = 8,  tuple(toUInt32(8),  toUInt32(8),  toUInt32(8)),
        x = 9,  tuple(toUInt32(9),  toUInt32(9),  toUInt32(9)),
        x = 10, tuple(toUInt32(10), toUInt32(10), toUInt32(10)),
        x = 11, tuple(toUInt32(11), toUInt32(11), toUInt32(11)),
        x = 12, tuple(toUInt32(12), toUInt32(12), toUInt32(12)),
        x = 13, tuple(toUInt32(13), toUInt32(13), toUInt32(13)),
        x = 14, tuple(toUInt32(14), toUInt32(14), toUInt32(14)),
        x = 15, tuple(toUInt32(15), toUInt32(15), toUInt32(15)),
        x = 16, tuple(toUInt32(16), toUInt32(16), toUInt32(16)),
        x = 17, tuple(toUInt32(17), toUInt32(17), toUInt32(17)),
        x = 18, tuple(toUInt32(18), toUInt32(18), toUInt32(18)),
        x = 19, tuple(toUInt32(19), toUInt32(19), toUInt32(19)),
        x = 20, tuple(toUInt32(20), toUInt32(20), toUInt32(20)),
        x = 21, tuple(toUInt32(21), toUInt32(21), toUInt32(21)),
        x = 22, tuple(toUInt32(22), toUInt32(22), toUInt32(22)),
        x = 23, tuple(toUInt32(23), toUInt32(23), toUInt32(23)),
        x = 24, tuple(toUInt32(24), toUInt32(24), toUInt32(24)),
        x = 25, tuple(toUInt32(25), toUInt32(25), toUInt32(25)),
        x = 26, tuple(toUInt32(26), toUInt32(26), toUInt32(26)),
        x = 27, tuple(toUInt32(27), toUInt32(27), toUInt32(27)),
        x = 28, tuple(toUInt32(28), toUInt32(28), toUInt32(28)),
        x = 29, tuple(toUInt32(29), toUInt32(29), toUInt32(29)),
        x = 30, tuple(toUInt32(30), toUInt32(30), toUInt32(30)),
        x = 31, tuple(toUInt32(31), toUInt32(31), toUInt32(31)),
        x = 32, tuple(toUInt32(32), toUInt32(32), toUInt32(32)),
        x = 33, tuple(toUInt32(33), toUInt32(33), toUInt32(33)),
        x = 34, tuple(toUInt32(34), toUInt32(34), toUInt32(34)),
        x = 35, tuple(toUInt32(35), toUInt32(35), toUInt32(35)),
        x = 36, tuple(toUInt32(36), toUInt32(36), toUInt32(36)),
        x = 37, tuple(toUInt32(37), toUInt32(37), toUInt32(37)),
        x = 38, tuple(toUInt32(38), toUInt32(38), toUInt32(38)),
        x = 39, tuple(toUInt32(39), toUInt32(39), toUInt32(39)),
        x = 40, tuple(toUInt32(40), toUInt32(40), toUInt32(40)),
        x = 41, tuple(toUInt32(41), toUInt32(41), toUInt32(41)),
        x = 42, tuple(toUInt32(42), toUInt32(42), toUInt32(42)),
        x = 43, tuple(toUInt32(43), toUInt32(43), toUInt32(43)),
        x = 44, tuple(toUInt32(44), toUInt32(44), toUInt32(44)),
        x = 45, tuple(toUInt32(45), toUInt32(45), toUInt32(45)),
        x = 46, tuple(toUInt32(46), toUInt32(46), toUInt32(46)),
        x = 47, tuple(toUInt32(47), toUInt32(47), toUInt32(47)),
        x = 48, tuple(toUInt32(48), toUInt32(48), toUInt32(48)),
        x = 49, tuple(toUInt32(49), toUInt32(49), toUInt32(49)),
        x = 50, tuple(toUInt32(50), toUInt32(50), toUInt32(50)),
        x = 51, tuple(toUInt32(51), toUInt32(51), toUInt32(51)),
        x = 52, tuple(toUInt32(52), toUInt32(52), toUInt32(52)),
        x = 53, tuple(toUInt32(53), toUInt32(53), toUInt32(53)),
        x = 54, tuple(toUInt32(54), toUInt32(54), toUInt32(54)),
        x = 55, tuple(toUInt32(55), toUInt32(55), toUInt32(55)),
        x = 56, tuple(toUInt32(56), toUInt32(56), toUInt32(56)),
        x = 57, tuple(toUInt32(57), toUInt32(57), toUInt32(57)),
        x = 58, tuple(toUInt32(58), toUInt32(58), toUInt32(58)),
        x = 59, tuple(toUInt32(59), toUInt32(59), toUInt32(59)),
        x = 60, tuple(toUInt32(60), toUInt32(60), toUInt32(60)),
        x = 61, tuple(toUInt32(61), toUInt32(61), toUInt32(61)),
        x = 62, tuple(toUInt32(62), toUInt32(62), toUInt32(62)),
        x = 63, tuple(toUInt32(63), toUInt32(63), toUInt32(63)),
        x = 64, tuple(toUInt32(64), toUInt32(64), toUInt32(64)),
        x = 65, tuple(toUInt32(65), toUInt32(65), toUInt32(65)),
        x = 66, tuple(toUInt32(66), toUInt32(66), toUInt32(66)),
        x = 67, tuple(toUInt32(67), toUInt32(67), toUInt32(67)),
        x = 68, tuple(toUInt32(68), toUInt32(68), toUInt32(68)),
        x = 69, tuple(toUInt32(69), toUInt32(69), toUInt32(69)),
        x = 70, tuple(toUInt32(70), toUInt32(70), toUInt32(70)),
        x = 71, tuple(toUInt32(71), toUInt32(71), toUInt32(71)),
        x = 72, tuple(toUInt32(72), toUInt32(72), toUInt32(72)),
        x = 73, tuple(toUInt32(73), toUInt32(73), toUInt32(73)),
        x = 74, tuple(toUInt32(74), toUInt32(74), toUInt32(74)),
        x = 75, tuple(toUInt32(75), toUInt32(75), toUInt32(75)),
        x = 76, tuple(toUInt32(76), toUInt32(76), toUInt32(76)),
        x = 77, tuple(toUInt32(77), toUInt32(77), toUInt32(77)),
        x = 78, tuple(toUInt32(78), toUInt32(78), toUInt32(78)),
        x = 79, tuple(toUInt32(79), toUInt32(79), toUInt32(79)),
        x = 80, tuple(toUInt32(80), toUInt32(80), toUInt32(80)),
        x = 81, tuple(toUInt32(81), toUInt32(81), toUInt32(81)),
        x = 82, tuple(toUInt32(82), toUInt32(82), toUInt32(82)),
        x = 83, tuple(toUInt32(83), toUInt32(83), toUInt32(83)),
        x = 84, tuple(toUInt32(84), toUInt32(84), toUInt32(84)),
        x = 85, tuple(toUInt32(85), toUInt32(85), toUInt32(85)),
        x = 86, tuple(toUInt32(86), toUInt32(86), toUInt32(86)),
        x = 87, tuple(toUInt32(87), toUInt32(87), toUInt32(87)),
        x = 88, tuple(toUInt32(88), toUInt32(88), toUInt32(88)),
        x = 89, tuple(toUInt32(89), toUInt32(89), toUInt32(89)),
        x = 90, tuple(toUInt32(90), toUInt32(90), toUInt32(90)),
        x = 91, tuple(toUInt32(91), toUInt32(91), toUInt32(91)),
        x = 92, tuple(toUInt32(92), toUInt32(92), toUInt32(92)),
        x = 93, tuple(toUInt32(93), toUInt32(93), toUInt32(93)),
        x = 94, tuple(toUInt32(94), toUInt32(94), toUInt32(94)),
        x = 95, tuple(toUInt32(95), toUInt32(95), toUInt32(95)),
        x = 96, tuple(toUInt32(96), toUInt32(96), toUInt32(96)),
        x = 97, tuple(toUInt32(97), toUInt32(97), toUInt32(97)),
        x = 98, tuple(toUInt32(98), toUInt32(98), toUInt32(98)),
        x = 99, tuple(toUInt32(99), toUInt32(99), toUInt32(99)),
                tuple(toUInt32(0),  toUInt32(0),  toUInt32(0))
    )
) ENGINE = Memory;

-- INSERT a few rows. The hot path is `getSampleBlock` -> analyze the MATERIALIZED
-- expression -> `resolveFunction` -> `getTreeHash`, the same path the issue points at.
INSERT INTO t_105351 (x) SELECT number FROM numbers(10) SETTINGS enable_analyzer = 1;

SELECT count() FROM t_105351;

-- Re-run the same INSERT through `VALUES` to cover that interpreter entry point.
-- `enable_analyzer = 1` is the default in current versions; in-statement
-- `SETTINGS` is not accepted after `VALUES` (the parser cannot disambiguate
-- additional values from a settings clause).
INSERT INTO t_105351 (x) VALUES (1000);

SELECT count() FROM t_105351;

-- Confirm SELECT of the materialized column reads stored values (unchanged path).
-- `x = 1000` falls through to the default arm of the `multiIf` above (no explicit
-- arm matches), so the expected `tupleElement(big, 1)` is 0.
SELECT tupleElement(big, 1) AS a FROM t_105351 WHERE x = 1000 SETTINGS enable_analyzer = 1;

DROP TABLE t_105351;
