describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Array.prototype.toSorted).toHaveLength(1);
    });

    test("basic functionality", () => {
        const a = [2, 4, 1, 3, 5];
        const b = a.toSorted();
        expect(a).not.toBe(b);
        expect(a).toEqual([2, 4, 1, 3, 5]);
        expect(b).toEqual([1, 2, 3, 4, 5]);
    });

    test("custom compare function", () => {
        const a = [2, 4, 1, 3, 5];
        const b = a.toSorted(() => 0);
        expect(a).not.toBe(b);
        expect(a).toEqual([2, 4, 1, 3, 5]);
        expect(b).toEqual([2, 4, 1, 3, 5]);
    });

    test("keeps temporary objects alive during a reentrant comparator", () => {
        const length = 67;
        const items = new Proxy(
            { length },
            {
                get(target, property) {
                    if (property === "length") return target.length;
                    return { key: (Number(property) * 17) % length };
                },
            }
        );
        let calls = 0;
        const sorted = Array.prototype.toSorted.call(items, (a, b) => {
            if (++calls % 13 === 0) {
                gc();
                expect([2, 1].sort()).toEqual([1, 2]);
            }
            return a.key - b.key;
        });
        expect(sorted.map(item => item.key)).toEqual(Array.from({ length }, (_, i) => i));
    });

    test("is unscopable", () => {
        expect(Array.prototype[Symbol.unscopables].toSorted).toBeTrue();
        const array = [];
        with (array) {
            expect(() => {
                toSorted;
            }).toThrowWithMessage(ReferenceError, "'toSorted' is not defined");
        }
    });
});

describe("errors", () => {
    test("null or undefined this value", () => {
        expect(() => {
            Array.prototype.toSorted.call();
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

        expect(() => {
            Array.prototype.toSorted.call(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

        expect(() => {
            Array.prototype.toSorted.call(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("invalid compare function", () => {
        expect(() => {
            [].toSorted("foo");
        }).toThrowWithMessage(TypeError, "foo is not a function");
    });
});
