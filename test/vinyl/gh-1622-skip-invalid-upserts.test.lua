s = box.schema.space.create('test', { engine = 'vinyl', field_count = 2 })
pk = s:create_index('pk')
s:replace{1, 1}
-- Error is logged, upsert is not applied.
--
s:upsert({1, 1}, {{'=', 3, 5}})
-- During read the incorrect upsert is ignored.
--
s:select{}

-- Try to set incorrect field_count in a transaction.
--
box.begin()
s:replace{2, 2}
s:upsert({2, 2}, {{'=', 3, 2}})
s:select{}
box.commit()
s:select{}

-- Read incorrect upsert from a run: it should be ignored.
--
box.snapshot()
s:select{}
s:upsert({2, 2}, {{'=', 3, 20}})
box.snapshot()
s:select{}

-- Execute replace/delete after invalid upsert.
--
box.snapshot()
s:upsert({2, 2}, {{'=', 3, 30}})
s:replace{2, 3}
s:select{}

s:upsert({1, 1}, {{'=', 3, 30}})
s:delete{1}
s:select{}

-- Invalid upsert in a sequence of upserts is skipped meanwhile
-- the rest are applied.
--
box.snapshot()
s:upsert({2, 2}, {{'+', 2, 5}})
s:upsert({2, 2}, {{'=', 3, 40}})
s:upsert({2, 2}, {{'+', 2, 5}})
s:select{}
box.snapshot()
s:select{}

-- Try to insert too many invalid upserts in a row so that
-- they trigger squashing procedure.
--
fiber = require('fiber')
s:drop()

s = box.schema.space.create('test', { engine = 'vinyl', field_count = 2 })
pk = s:create_index('pk')
s:replace({1, 1})

squashed_before = pk:stat().upsert.squashed
applied_before = pk:stat().upsert.applied
len_before = s:len()
upsert_cnt = 129
for i =1 , upsert_cnt do s:upsert({1, 1}, {{'=', 3, 5}}) end
-- 1 squash is accounted in vy_squash_process() unconditionally.
--
while pk:stat().upsert.squashed ~= squashed_before + 1 do fiber.sleep(0.01) end
-- Make sure invalid upserts are committed into tree.
--
assert(s:len() == len_before + upsert_cnt)
assert(pk:stat().upsert.applied == applied_before)
-- Adding new invalid upserts will not result in squash until
-- we reach VY_UPSERT_THRESHOLD (128) limit again.
--
s:upsert({1, 1}, {{'=', 3, 5}})
assert(pk:stat().upsert.squashed == squashed_before + 1)
for i = 1, upsert_cnt - 3 do s:upsert({1, 1}, {{'=', 3, 5}}) end
assert(pk:stat().upsert.squashed == squashed_before + 1)
s:upsert({1, 1}, {{'=', 3, 5}})
while pk:stat().upsert.squashed ~= squashed_before + 2 do fiber.sleep(0.01) end
assert(pk:stat().upsert.squashed == squashed_before + 2)
assert(pk:stat().upsert.applied == applied_before)
box.snapshot()
-- No one upsert is applied so number of rows should not change.
--
assert(s:len() == len_before)
assert(pk:stat().upsert.squashed == squashed_before + 2)
assert(pk:stat().upsert.applied == applied_before)
-- Now let's mix valid and invalid upserts. Make sure that only
-- valid are accounted during squash/apply.
--
for i = 1, upsert_cnt do if i % 2 == 0 then s:upsert({1, 1}, {{'=', 3, 5}}) else s:upsert({1, 1}, {{'+', 2, 1}}) end end
while pk:stat().upsert.squashed ~= squashed_before + 3 do fiber.sleep(0.01) end
assert(s:len() == len_before + upsert_cnt)
assert(pk:stat().upsert.applied == math.floor(upsert_cnt/2) + 1)
box.snapshot()
assert(s:len() == len_before)
assert(pk:stat().upsert.squashed == squashed_before + 3)
assert(pk:stat().upsert.applied == math.floor(upsert_cnt/2) + 1)

s:drop()