# Bits: count the 1 bits in every number below 300,000.
ones = 0
for n in range(300000):
    v = n
    while v != 0:
        v = v & (v - 1)
        ones += 1
print(ones)
