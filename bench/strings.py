# Text: build 20,000 short messages, as a program sending readings would.
chars = 0
for i in range(20000):
    msg = "reading {}: {} C".format(i, i % 40)
    chars += len(msg)
print(chars)
