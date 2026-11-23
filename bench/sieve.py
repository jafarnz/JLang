# Memory: the primes below 200,000, one byte per number.
N = 200000
composite = bytearray(N)
count = 0
for i in range(2, N):
    if composite[i] == 0:
        count += 1
        j = i * 2
        while j < N:
            composite[j] = 1
            j += i
print(count)
