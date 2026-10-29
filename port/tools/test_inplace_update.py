"""Compatibility entry point for the clean in-place updater regression."""
import sys
from pathlib import Path
from test_clean_update import main
if __name__ == '__main__':
    sys.argv=['--payload' if value=='--payload-dir' else value for value in sys.argv]
    if '--payload' in sys.argv:
        index=sys.argv.index('--payload')+1
        if Path(sys.argv[index]).name=='bin':sys.argv[index]=str(Path(sys.argv[index]).parent)
    main()
