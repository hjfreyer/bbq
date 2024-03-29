"use client";



import { collection, doc, query, limit, orderBy, Firestore, getDoc, onSnapshot, Timestamp, updateDoc } from 'firebase/firestore';
import uPlot, { AlignedData, Axis, Options } from 'uplot';
import '@/node_modules/uplot/dist/uPlot.min.css';
import * as _ from "lodash";
import { FormEvent, useEffect, useRef, useState } from 'react';
import Link from 'next/link';

import { db } from '@/lib/firebase';

type Sample = {
    time: Timestamp,
    probe_temps_f: number[],
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

const MultilineChart = ({ data, food_diffs, dimensions }: { data: Data, food_diffs: number[], dimensions: Dimensions }) => {
    const svgRef = useRef<HTMLDivElement>(null);

    let opts: Options = {
        title: "LET THERE BE FOOD",
        class: "my-chart",
        width: 800,
        height: 600,
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
                labelGap: 8,
                labelSize: 8 + 12 + 8,
                stroke: "red",
            },
            {
                side: 1,
                scale: 'diff',
                label: '°F/min'
            }
        ],

    };
    const chart = useRef(new uPlot(opts, [[], []]));

    useEffect(() => {
        let times = data.map(datum => datum.time.toDate().getTime() / 1000);
        let y_values = data.map(datum => datum.probe_temps_f[0]);
        let y2_values = data.map(datum => datum.probe_temps_f[1]);
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

    }, [data])

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
    samples: Sample[],
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
        return onSnapshot(doc(db, "sessions-1", id), {
            next: (d) => {
                let session = d.data() as Session;

                session.samples = _.sortBy(session.samples, 'time.seconds');

                setSession(session)
            }
        });
    }, [id]);


    useEffect(() => {
        return onSnapshot(query(collection(db, "sessions-1"),
            orderBy('last_update', 'desc'),
            limit(1)), {
            next: (coll) => {
                setLatestId(coll.docs[0].ref.id)
            }
        });
    }, []);

    if (!session) {
        return <div>Not found!</div>
    }

    let notLatest = id == latestId ? <div /> : <div className='bg-orange-400 m-1 p-1 rounded'>This is not the latest session. <Link href={"/sessions/" + latestId}>Go to latest session</Link></div>

    return (<main className='p-4'>
        {notLatest}
        <SessionView id={id} session={session} />
    </main>)
}

export type SessionViewProps = { id: string, session: Session };

function SessionView({ id, session }: SessionViewProps) {
    const [formTarget, setFormTarget] = useState(session.food_target);

    if (session === null) {
        return <div>Loading? Not Found?</div>;
    }

    let temps = session.samples;
    let diffs = [];
    for (let i = 0; i < temps.length; i++) {
        const sample = temps[i];
        let begin = _.sortedIndexBy(temps, { time: { seconds: sample.time.seconds - 10 * 60 } } as Sample, 'time.seconds');

        diffs.push((sample.probe_temps_f[1] - temps[begin].probe_temps_f[1]) / 10);
    }


    let target = session.food_target;
    let estimate = "";
    let doneat = "";
    if (temps.length > 1) {
        const begin = Math.max(temps.length - 21, 0);
        const end = temps.length - 1;
        const rate = diffs[end];
        const delta = (target - temps[end].probe_temps_f[1]) / rate * 60;
        estimate = delta.toFixed(0) + " seconds";
        doneat = "" + new Date(new Date().getTime() + delta * 1000);
    }

    const setTarget = (e: FormEvent) => {
        e.preventDefault();
        updateDoc(doc(db, "sessions-1", id), {
            food_target: formTarget
        })
    };

    return <div>
        <form onSubmit={setTarget}>
            <label htmlFor="name">Target Temperature</label>
            <input
                id="name"
                type="number"
                value={formTarget}
                onChange={(e) => setFormTarget(+e.target.value)}
            />
            <button type="submit">Submit</button>
        </form>


        Time left: {estimate}<br />
        Done at: {doneat}<br />
        Ambient: {temps[temps.length - 1]?.probe_temps_f[0].toFixed(1)}°F<br />
        Food: {temps[temps.length - 1]?.probe_temps_f[1].toFixed(1)}°F<br />
        <MultilineChart dimensions={{
            width: 200,
            height: 200,
            margin: {
                top: 10,
                left: 10,
                right: 10,
                bottom: 10

            }
        }}
            data={session.samples}
            food_diffs={diffs}
        />
    </div>
}
