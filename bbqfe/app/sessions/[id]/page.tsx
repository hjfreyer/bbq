"use client";



import { collection, doc, query, limit, orderBy, Firestore, getDoc, onSnapshot, Timestamp, updateDoc } from 'firebase/firestore';
import uPlot, { AlignedData, Axis, Options } from 'uplot';
import '@/node_modules/uplot/dist/uPlot.min.css';
import * as _ from "lodash";
import { FormEvent, useEffect, useRef, useState, useLayoutEffect } from 'react';
import Link from 'next/link';

import { db } from '@/lib/firebase';

type Sample = {
    time: Timestamp,
    ambient_temp_f: number,
    food_temp_f: number,
    duty_pct: number,
};

type Data = Sample[];

type Dimensions = {
    width: number,
    height: number,
    margin: {
        top: number,
        left: number,
        right: number,
        bottom: number
    },
}

const MultilineChart = ({ data, food_diffs }: { data: Data, food_diffs: number[] }) => {
    const [width, setWidth] = useState(0);
    const svgRef = useRef<HTMLDivElement>(null);
    
    useLayoutEffect(() => {
        function updateSize() {
            if (svgRef.current) {
                setWidth(svgRef.current.clientWidth);
            }
        }
        window.addEventListener('resize', updateSize);
        updateSize();
        return () => window.removeEventListener('resize', updateSize);
    }, [svgRef]);

    let opts: Options = {
        title: "LET THERE BE FOOD",
        class: "my-chart",
        width: 0,
        height: 0,
        scales: {
            diff: {}
        },
        series: [
            {
                auto: false,
                // range: (min, max) => [1566453600, 1566813540], 
            },
            {
                // initial toggled state (optional)
                show: true,

                spanGaps: false,

                // in-legend display
                label: "Ambient",
                value: (self, rawValue) => rawValue + '°F',

                // series style
                stroke: "red",
                width: 1,
                // fill: "rgba(255, 0, 0, 0.3)",
                // dash: [10, 5],
            },
            {
                // initial toggled state (optional)
                show: true,

                spanGaps: false,

                // in-legend display
                label: "Food",
                value: (self, rawValue) => rawValue + '°F',

                // series style
                stroke: "blue",
                width: 1,
                // fill: "rgba(255, 0, 0, 0.3)",
                // dash: [10, 5],
            },
            {
                show: true,
                stroke: "green",
                width: 1,
                scale: 'diff',
                value: (self, rawValue) => rawValue && (rawValue.toFixed(2) + '°F/min'),
                label: "Cook Rate"

            }
        ],
        axes: [
            {
                label: "Time",
            },
            {
                label: "Temperature",
                labelGap: 4,
                labelSize: 4 + 12 + 4,
                stroke: "red",
            },
            {
                side: 1,
                scale: 'diff',
                label: '°F/min',
                labelGap: 4,
                labelSize: 4 + 12 + 4,
            }
        ],

    };
    const chart = useRef(new uPlot(opts, [[], []]));

    useEffect(() => {
        let times = data.map(datum => datum.time.toDate().getTime() / 1000);
        let y_values = data.map(datum => datum.ambient_temp_f);
        let y2_values = data.map(datum => datum.food_temp_f);
        let y2_diff = food_diffs;
        // data.map((datum, i) => {
        //     if (i == 0) {
        //         return 0;
        //     }
        //     let start = Math.max(i - 20, 0);
        //     return (datum.probe_temps_f[1] - data[start].probe_temps_f[1]) / (times[i] - times[start]) * 60
        // });
        let data2: AlignedData = [
            times, y_values, y2_values, y2_diff,
        ];
        if (chart.current === undefined) {
            return
        }
        const c = chart.current;
        c.setData(data2, false);

        let prev_scale = c.scales['x'];
        if (prev_scale.min == null && data2[0].length > 0) {
            let min = Math.min(...times);
            let max = Math.max(...times);
            c.setScale('x', { min, max });
        }
        if (prev_scale.min != null) {
            let max = Math.max(...times);
            c.setScale('x', { min: prev_scale.min!, max });
        }

    }, [data, food_diffs]);

    useEffect(()=>{
        chart.current.setSize({width: width, height: width * 3/ 4});
    }, [width]);

    useEffect(() => {
        if (svgRef.current != null && svgRef.current.childElementCount == 0) {
            svgRef.current.appendChild(chart.current.root)
            return
        }
    }, [svgRef])

    return <div ref={svgRef} />;
};

export type Session = {
    last_update: Timestamp,
    food_target: number,
};

export type SessionViewWrapperProps = {
    params: { id: string }
    // by_date: [string, Session][],
}


export default function SessionViewWrapper({ params }: SessionViewWrapperProps) {
    const { id } = params;

    // const [sessions, setSessions] = useState<[string, Session][]>([]);

    // useEffect(() => {
    //     onSnapshot(collection(db, "sessions-1"), {
    //         next: (coll) => {


    //             setSessions(coll.docs.map(doc => {
    //                 const session = doc.data() as Session;

    //                 session.samples = _.sortBy(session.samples, 'time.seconds')

    //                 return [doc.id, session];
    //             }))
    //         }
    //     });
    // }, []);


    // useEffect(() => {
    //     return onSnapshot(query(collection(db, "sessions-1"),
    //                      orderBy('last_update', 'desc'),
    //                     limit(1)), {
    //         next: (coll) => {


    //             setSessions(coll.docs.map(doc => {
    //                 const session = doc.data() as Session;

    //                 // session.samples = _.sortBy(session.samples, 'time.seconds')

    //                 return [doc.id, session];
    //             }))
    //         }
    //     });
    // }, []);


    const [session, setSession] = useState<Session | null>(null);
    const [samples, setSamples] = useState<Sample[]>([]);

    const [latestId, setLatestId] = useState<string>(id);



    // useEffect(() => {
    //     onSnapshot(collection(db, "sessions-1"), {
    //         next: (coll) => {


    //             setSessions(coll.docs.map(doc => {
    //                 const session = doc.data() as Session;

    //                 session.samples = _.sortBy(session.samples, 'time.seconds')

    //                 return [doc.id, session];
    //             }))
    //         }
    //     });
    // }, []);


    useEffect(() => {
        return onSnapshot(doc(db, "sessions-2", id), {
            next: (d) => {
                let session = d.data() as Session;
                setSession(session)
            }
        });
    }, [id]);


    useEffect(() => {
        return onSnapshot(collection(db, "sessions-2", id, "readings"), {
            next: (d) => {
                const samples: Sample[] = d.docs.map(d => (d.data() as Sample));
                setSamples(_.sortBy(samples, 'time.seconds'))
            }
        });
    }, [id]);


    useEffect(() => {
        return onSnapshot(query(collection(db, "sessions-2"),
            orderBy('last_update', 'desc'),
            limit(1)), {
            next: (coll) => {
                setLatestId(coll.docs[0].ref.id)
            }
        });
    }, []);

    if (!session) {
        return <div className='p-4'>Loading...</div>
    }

    let notLatest = id == latestId ? <div /> : <div className='bg-orange-400 m-1 p-1 rounded'>This is not the latest session. <Link href={"/sessions/" + latestId}>Go to latest session</Link></div>

    return (<main className='p-4'>
        {notLatest}
        <SessionView id={id} session={session} samples={samples} />
    </main>)
}

export type SessionViewProps = { id: string, session: Session, samples: Sample[] };

function SessionView({ id, session, samples }: SessionViewProps) {
    const [formTarget, setFormTarget] = useState(session.food_target);

    if (session === null) {
        return <div>Loading? Not Found?</div>;
    }

    const times: Timestamp[] = samples.map((s) => s.time);
    const ambients: number[] = movingAverage(times, samples.map((s) => s.ambient_temp_f), 20);
    const foods: number[] = movingAverage(times, samples.map((s) => s.food_temp_f), 100);
    const diffs = movingAverage(times, diff(times, foods), 500).map((x) => 60 * x);

    let target = session.food_target;
    let estimate = "";
    let doneat = "";
    if (times.length > 1) {
        const end = times.length - 1;
        const rate = diffs[end];
        const delta = (target - foods[end]) / rate * 60;
        estimate = delta.toFixed(0) + " seconds";
        doneat = "" + new Date(new Date().getTime() + delta * 1000);
    }

    const setTarget = (e: FormEvent) => {
        e.preventDefault();
        updateDoc(doc(db, "sessions-2", id), {
            food_target: formTarget
        })
    };

    const temps: Sample[] = [];
    for (let i = 0; i < times.length; i++) {
        temps.push({
            time: times[i],
            ambient_temp_f: ambients[i],
            food_temp_f: foods[i],
            duty_pct: samples[i].food_temp_f
        })
    }

    return <div>
        <div className="grid grid-cols-2">
            <label className="font-semibold" htmlFor="name">Target Temperature</label>
            <form onSubmit={setTarget}>
                <input
                    id="name"
                    type="number"
                    value={formTarget}
                    onChange={(e) => setFormTarget(+e.target.value)}
                />
                <button type="submit" className="bg-blue-500 hover:bg-blue-700 text-white font-bold px-2 mx-1 rounded" >Submit</button>
            </form>

            <div className='font-semibold'>Time left</div>
            <div>{estimate}</div>
            <div className='font-semibold'>Done at</div>
            <div>{doneat}</div>
            <div className='font-semibold'>Ambient</div>
            <div>{ambients[ambients.length - 1]?.toFixed(1)}°F</div>
            <div className='font-semibold'>Food</div>
            <div>{foods[foods.length - 1]?.toFixed(1)}°F</div>
        </div>
        <MultilineChart 
            data={temps}
            food_diffs={diffs}
        />
    </div>
}

function movingAverage(times: Timestamp[], data: number[], tau: number): number[] {
    const res = [data[0]];
    for (let i = 1; i < data.length; i++) {
        let coeff = 1 - Math.exp(-(times[i].seconds - times[i - 1].seconds) / tau);
        res.push(res[i - 1] + coeff * (data[i] - res[i - 1]));
    }
    return res;
}

function diff(times: Timestamp[], data: number[]): number[] {
    const res = [0];
    for (let i = 1; i < data.length; i++) {
        const delta_t = times[i].seconds - times[i - 1].seconds;
        if (delta_t === 0) {
            res.push(0);
        } else {
            res.push((data[i] - data[i - 1]) / delta_t);
        }
    }
    return res;
}