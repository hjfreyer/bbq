'use client'
// import React, { useState, useEffect } from "react";
import Link from "next/link";

export default function Header({ }) {
	return (
		<header>
			<Link href="/" className="logo">
				<h1 className="text-3xl p-4">BBQBOT HQ</h1>
			</Link>
		</header>
	);
}