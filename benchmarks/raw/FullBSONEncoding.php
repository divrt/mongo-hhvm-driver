<?php
namespace MongoDB\Benchmark\Raw;
use \MongoDB\Benchmark\BSONEncoding;
use \MongoDB\Benchmark\Task;

class FullBSONEncoding extends BSONEncoding implements Task
{
	protected $data;

	function setup()
	{
		$this->data = $this->loadFile( "../data/full_bson.json" );	
	}

	function beforeTask()
	{
	}

	function doTask()
	{
		for ( $i = 0; $i < 10000; $i++ )
		{
			$encoded = \MongoDB\BSON\fromPHP( $this->data );
		}
	}

	function afterTask()
	{
	}

	function tearDown()
	{
	}
}
?>
