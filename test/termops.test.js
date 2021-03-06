var assert = require('assert'),
    termops = require('../lib/util/termops');

describe('termops', function() {
    describe('tokenize', function() {
        describe('examples', function() {
            it('tokenizes basic strings', function() {
                assert.deepEqual(termops.tokenize('foo'), ['foo']);
                assert.deepEqual(termops.tokenize('foo bar'), ['foo', 'bar']);
                assert.deepEqual(termops.tokenize('foo-bar'), ['foo', 'bar']);
                assert.deepEqual(termops.tokenize('San José'), ['san', 'jose']);
                assert.deepEqual(termops.tokenize('San José'), ['san', 'jose']);
                assert.deepEqual(termops.tokenize('Chamonix-Mont-Blanc'), ['chamonix','mont','blanc']);
                assert.deepEqual(termops.tokenize('Москва'), ['moskva']);
                assert.deepEqual(termops.tokenize('京都市'), ['jing','du','shi']);
            });
            it('tokenizes lonlat', function() {
                assert.deepEqual(termops.tokenize('40,0', true), [40,0]);
                assert.deepEqual(termops.tokenize('-120.9129102983109, 45.312312', true), [-120.9129102983109,45.312312]);
                assert.deepEqual(termops.tokenize('14th 15th', true), ['14th','15th']);
            });
        });
        describe('edge cases', function() {
            it('empty string', function() {
                assert.deepEqual(termops.tokenize(''), []);
            });
        });
    });
    describe('terms', function() {
        it('tokenizes and hashes values', function() {
            assert.deepEqual(termops.terms(['foo','bar']), [2851307216,1991736592]);
        });
    });
    describe('termsMap', function() {
        it('tokenizes and hashes values', function() {
            assert.deepEqual(termops.termsMap(['foo','bar']), {
                2851307216: 'foo',
                1991736592: 'bar'
            });
        });
    });
    describe('degens', function() {
        it('generates degenerates', function() {
            var degens = termops.degens('foobarbaz');
            assert.deepEqual(degens, [
                1617781328, 1617781328,
                4112850176, 1617781329,
                2921073328, 1617781330,
                3214735712, 1617781331,
                967483776, 1617781332,
                1062237920, 1617781333,
                2851307216, 1617781334
            ]);
            for (var i = 0; i < degens.length/2; i = i + 2) {
                // Encodes ID for 'foobarbaz'.
                assert.equal(degens[i+1] >>> 4 << 4 >>> 0, termops.terms(['foobarbaz'])[0]);
                // Encodes degen distance (max: 15) from foobarbaz.
                assert.ok(degens[i+1] % 16 <= 15);
            }
        });
    });
    describe('phrase', function() {
        it('generates a name id', function() {
            assert.deepEqual(termops.phrase(['foo'], 'foo'), 2851307223);
            assert.deepEqual(termops.phrase(['foo','street'], 'foo'), 2851505742);
            assert.deepEqual(termops.phrase(['foo','lane'], 'foo'), 2851502143);
            // Clusters phrase IDs based on initial term.
            assert.deepEqual(termops.phrase(['foo'], 'foo') >>> 24, 169);
            assert.deepEqual(termops.phrase(['foo','street'], 'foo') >>> 24, 169);
            assert.deepEqual(termops.phrase(['foo','lane'], 'foo') >>> 24, 169);
        });
    });
});
