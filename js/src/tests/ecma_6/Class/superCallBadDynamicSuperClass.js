var test = `

class base { constructor() { } }

class inst extends base { constructor() { super(); } }

Object.setPrototypeOf(inst, Math.sin);

assertThrowsInstanceOf(() => new inst(), TypeError);

`;

if (classesEnabled())
    eval(test);

if (typeof reportCompare === 'function')
    reportCompare(0,0,"OK");
