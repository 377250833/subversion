import sys, os
bindir = os.path.dirname(sys.argv[0])
sys.path[0:0] = [ os.getcwd(), "%s/.libs" % os.getcwd(), \
                  "%s/.." % bindir, "%s/../.libs" % bindir ]
import unittest
import pool

# Run all tests

def suite():
  """Run all tests"""
  suite = unittest.TestSuite()
  suite.addTest(pool.suite())
  return suite

if __name__ == '__main__':
  unittest.main(defaultTest='suite')
