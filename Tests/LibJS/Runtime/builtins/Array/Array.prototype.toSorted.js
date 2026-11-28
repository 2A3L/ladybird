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

    test("reads inherited elements and subsequent indices up to the captured length", () => {
        const source = [4];
        source.length = 4;
        Object.setPrototypeOf(source, {
            get 1() {
                source[2] = 2;
                source[4] = 0;
                return 1;
            },
        });
        expect(Array.prototype.toSorted.call(source)).toEqual([1, 2, 4, undefined]);
    });

    test("reads every index through a proxy around a packed array", () => {
        const reads = [];
        const source = new Proxy([3, 2, 1], {
            get(target, key) {
                reads.push(key);
                return key === "0" ? 0 : target[key];
            },
        });
        expect(Array.prototype.toSorted.call(source)).toEqual([0, 1, 2]);
        expect(reads).toEqual(["length", "0", "1", "2"]);
    });

    test("custom compare function", () => {
        const a = [2, 4, 1, 3, 5];
        const b = a.toSorted(() => 0);
        expect(a).not.toBe(b);
        expect(a).toEqual([2, 4, 1, 3, 5]);
        expect(b).toEqual([2, 4, 1, 3, 5]);
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
