test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
box.sql.execute('pragma sql_default_engine=\''..engine..'\'')

-- gh-3735: make sure that integer overflows errors are
-- handled during VDBE execution.
--
box.sql.execute('SELECT (2147483647 * 2147483647 * 2147483647);')
box.sql.execute('SELECT (-9223372036854775808 / -1);')
box.sql.execute('SELECT (-9223372036854775808 - 1);')
box.sql.execute('SELECT (9223372036854775807 + 1);')
-- Literals are checked right after parsing.
--
box.sql.execute('SELECT 9223372036854775808;')
box.sql.execute('SELECT -9223372036854775809;')
box.sql.execute('SELECT 9223372036854775808 - 1;')
