// args: --emit tick --emit tick --emit chain
let count = 0
on tick {
    count += 1
    printf("tick {}\n", count)
}
on chain {
    emit("tick")
    print("chained")
}
print("loaded")
// expect: loaded
// expect: tick 1
// expect: tick 2
// expect: tick 3
// expect: chained
