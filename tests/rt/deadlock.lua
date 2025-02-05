-- Dew tries to detect deadlock situations; when a task would be suspended but never resumed.
-- For example, a task calling recv() without anyone send()ing to it.
-- It's impossible to detect all cases of deadlock; detection is best effort.
return function()
	__rt.timer_start(1*1000*1000, 0, 0)
	-- the following call to recv will not deadlock since there's a timer
	__rt.recv()

	-- this recv would deadlock and should cause an error
	local ok, err = pcall(function()
		__rt.recv()
	end)
	print("expecting error:", err)
	assert(not ok)
	assert(string.find(err, "deadlock", 1, true) ~= nil)
end
