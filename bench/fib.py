# Function calls: the 27th Fibonacci number, the slow recursive way.
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(27))
