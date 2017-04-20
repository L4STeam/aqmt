#!/usr/bin/env python3

# add path to library root
import sys
import os
sys.path.append(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))

from framework import Testbed, TestEnv, run_test, steps
from framework.plot import PlotAxis
from framework.traffic import greedy

def test(result_folder):

    def my_test(testcase):
        testcase.traffic(greedy, node='a', tag='CUBIC')
        testcase.traffic(greedy, node='b', tag='DCTCP')

    testbed = Testbed()

    testbed.ta_samples = 50
    testbed.ta_idle = 2
    testbed.ta_delay = 500

    testbed.cc('a', 'cubic', testbed.ECN_ALLOW)
    testbed.cc('b', 'dctcp-drop', testbed.ECN_INITIATE)

    run_test(
        folder=result_folder,
        title='Testing new pi2 API',
        testenv=TestEnv(testbed, retest=False),
        steps=(
            steps.plot_flows(level_order=[0,2]),
            steps.plot_compare(level_order=[0,2], x_axis=PlotAxis.LINEAR),
            steps.branch_sched([
                # tag, title, name, params
                ('pi2-1',
                    'PI^2',
                    'pi2', 'target 15ms tupdate 15ms alpha 5 beta 50 k 2 t_shift 30ms l_drop 100 l_thresh 3000'),
                ('pi2-2',
                    'PI^2: no\\\\_ecn',
                    'pi2', 'no_ecn target 15ms tupdate 15ms alpha 5 beta 50 k 2 t_shift 30ms l_drop 100 l_thresh 3000'),
                ('pi2-3',
                    'PI^2: no\\\\_dualq',
                    'pi2', 'no_dualq target 15ms tupdate 15ms alpha 5 beta 50 k 2 t_shift 30ms l_drop 100 l_thresh 3000'),
                ('pi2-4',
                    'PI^2: no\\\\_dualq no\\\\_ecn',
                    'pi2', 'no_dualq no_ecn target 15ms tupdate 15ms alpha 5 beta 50 k 2 t_shift 30ms l_drop 100 l_thresh 3000'),
                ('pi2-5',
                    'PI^2: dc\\\\_ecn',
                    'pi2', 'dc_ecn target 15ms tupdate 15ms alpha 5 beta 50 k 2 t_shift 30ms l_drop 100 l_thresh 3000'),
                ('pi2-6',
                    'PI^2: dc\\\\_dualq dc\\\\_ecn',
                    'pi2', 'dc_dualq dc_ecn target 15ms tupdate 15ms alpha 5 beta 50 k 2 t_shift 30ms l_drop 100 l_thresh 3000'),
            ]),
            steps.branch_rtt([10, 50, 100], title='%d'),
            steps.branch_bitrate([100]),
            #steps.skipif(lambda testenv: testenv.testdef.sched_tag != 'pi2-6' or testenv.testbed.rtt_servera != 100),
            my_test,
        )
    )

if __name__ == '__main__':
    result_folder = 'results/pi2-api'
    if len(sys.argv) >= 2:
        result_folder = sys.argv[1]

    test(result_folder)
