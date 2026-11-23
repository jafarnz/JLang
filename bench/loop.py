# A tight loop of arithmetic, like a filter running over samples.
total = 0
i = 0
while i < 3000000:
    total = (total + i * 7) & 0xFFFFFF
    i += 1
print(total)
